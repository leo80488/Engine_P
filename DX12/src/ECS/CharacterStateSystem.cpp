#include "ECS/CharacterStateSystem.h"
#include "ECS/CharacterStateComponent.h"
#include "ECS/AnimationComponents.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/SkeletonAsset.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>

namespace
{
float AdvanceStateTime(float time, float dt, float speed,
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
}

void CharacterStateSystem::Update(World& world, float dt, ClipLibrary& clipLib)
{
    auto* pCS = world.GetPool<CharacterStateComponent>();
    if (!pCS) return;

    auto& csEnts = pCS->Entities();
    auto& csData = pCS->Data();
    const size_t n = csData.size();

    for (size_t i = 0; i < n; ++i)
    {
        const Entity e = csEnts[i];
        if (!world.IsAlive(e)) continue;
        CharacterStateComponent& cs = csData[i];

        // Migrated states play through the AnimationSet / Animator path; the component
        // remains only for the Character.* management / query calls.
        if (cs.animatorMigrationComplete) continue;

        AnimationComponent* anim = world.GetComponent<AnimationComponent>(e);
        if (!anim) continue;
        AnimatorTransitionComponent* namedTransition =
            world.GetComponent<AnimatorTransitionComponent>(e);
        const SkeletonComponent* sc = world.GetComponent<SkeletonComponent>(e);
        if (!sc || sc->assetIndex == kInvalidAnimHandle) continue;
        const SkeletonAsset& skel = m_skeletons.Get(sc->assetIndex);

        // ---- 1. Lazy bind every state's clip ----
        // AcquireClip dedups by path and BindToSkeleton returns the cached ClipLibrary
        // index after the first bind; only states whose clipLibIdx is still invalid are
        // polled.
        if (m_clipSys)
        {
            for (auto& st : cs.states)
            {
                if (st.clipLibIdx != kInvalidClipIndex) continue;
                if (st.clipPath.empty())                continue;

                if (!st.clipResHandle.IsValid())
                    st.clipResHandle = m_clipSys->AcquireClip(st.clipPath);

                if (m_clipSys->IsReady(st.clipResHandle))
                {
                    const uint32_t idx =
                        m_clipSys->BindToSkeleton(st.clipResHandle, skel, clipLib);
                    if (idx != kInvalidClipIndex)
                    {
                        st.clipLibIdx = idx;
                        // ClipLibrary owns a bound copy. Keeping the resource handle in ECS
                        // would only leak it across World::Clear; playback does not need it.
                        m_clipSys->ReleaseClip(st.clipResHandle);
                        st.clipResHandle = {};
                    }
                }
            }
        }

        // ---- 2. Advance cross-fade ----
        if (cs.pendingIdx >= 0 &&
            static_cast<size_t>(cs.pendingIdx) < cs.states.size())
        {
            // Character.SetState is the playback authority for this transition:
            // cancel any pending name-based Animator.Play.
            world.RemoveComponent<AnimatorPlayRequestComponent>(e);
            if (namedTransition && namedTransition->active)
            {
                namedTransition->targetClipName.clear();
                namedTransition->active = false;
            }

            const auto& target = cs.states[cs.pendingIdx];
            if (target.clipLibIdx == kInvalidClipIndex)
                continue; // target not bound yet: wait

            // Initialise the secondary clip on the first frame of the blend.
            // SetState() only stamps pendingIdx and resets blendElapsed; the
            // AnimationComponent wiring happens here so this system owns the playback
            // fields.
            if (anim->secondaryClip != target.clipLibIdx)
            {
                anim->secondaryClip = target.clipLibIdx;
                anim->secondaryTime = 0.f;
            }
            // Two instant-snap cases promote secondary -> primary this frame
            // without a cross-fade:
            //   (a) a zero-blend SetState (OnSpawn boot, hard cuts);
            //   (b) primary is still kInvalidAnimHandle: the first state to bind on
            //       a fresh entity, queued before its clip loaded. AnimationSystem
            //       skips an entity with no primary clip, so blending would show the
            //       rest pose for the whole blendDuration while the NavAgent already
            //       moves it.
            if (cs.blendDuration <= 1e-4f ||
                anim->primaryClip == kInvalidAnimHandle)
            {
                anim->primaryClip   = anim->secondaryClip;
                anim->primaryTime   = anim->secondaryTime;
                anim->secondaryClip = kInvalidAnimHandle;
                anim->blendWeight   = 0.f;
                anim->looping       = target.looping;
                anim->speed         = target.playbackSpeed;
                cs.currentIdx       = cs.pendingIdx;
                cs.pendingIdx       = -1;
                cs.blendElapsed     = 0.f;
                if (namedTransition)
                {
                    namedTransition->currentClipName = target.name;
                    namedTransition->targetClipName.clear();
                    namedTransition->active = false;
                }
            }
            else
            {
                if (!anim->paused && target.clipLibIdx < clipLib.Count())
                {
                    const ClipAsset& targetClip = clipLib.Get(target.clipLibIdx);
                    anim->secondaryTime = AdvanceStateTime(
                        anim->secondaryTime, dt, target.playbackSpeed,
                        targetClip.duration, target.looping);
                }
                cs.blendElapsed += dt;
                const float t = std::min(1.0f, cs.blendElapsed / cs.blendDuration);
                anim->blendWeight = t;
                if (t >= 1.f)
                {
                    anim->primaryClip   = anim->secondaryClip;
                    anim->primaryTime   = anim->secondaryTime;
                    anim->secondaryClip = kInvalidAnimHandle;
                    anim->blendWeight   = 0.f;
                    anim->looping       = target.looping;
                    anim->speed         = target.playbackSpeed;
                    cs.currentIdx       = cs.pendingIdx;
                    cs.pendingIdx       = -1;
                    cs.blendElapsed     = 0.f;
                    if (namedTransition)
                    {
                        namedTransition->currentClipName = target.name;
                        namedTransition->targetClipName.clear();
                        namedTransition->active = false;
                    }
                }
            }
        }
        else if (cs.currentIdx >= 0 &&
                 static_cast<size_t>(cs.currentIdx) < cs.states.size())
        {
            // No pending blend: keep the primary clip current. Covers a current
            // state whose clip finished binding after the SetState that selected it.
            const auto& cur = cs.states[cs.currentIdx];
            if (cur.clipLibIdx != kInvalidClipIndex)
            {
                if (anim->primaryClip != cur.clipLibIdx)
                {
                    anim->primaryClip = cur.clipLibIdx;
                    anim->primaryTime = 0.f;
                }
                anim->looping     = cur.looping;
                anim->speed       = cur.playbackSpeed;
                if (namedTransition)
                    namedTransition->currentClipName = cur.name;
            }
        }
    }
}
