#include "ECS/CharacterAnimDriver.h"
#include "ECS/AnimGraphSystem.h"
#include "Gameplay/AttackPlayback.h"

#include "ECS/ECS.h"
#include "ECS/CharacterIntentComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/HierarchyComponents.h"     // LocalTransform / Children
#include "ECS/AnimationComponents.h"     // AnimationComponent / SkeletonRef
#include "ECS/AnimEntityResolve.h"       // Anim::ResolveAnimEntity (shared walk)
#include "ECS/LocomotionComponents.h"
#include "Resource/AnimationClipSystem.h"
#include "System/Log.h"
#include "System/Check.h"        // ENGINE_CHECK_ONCE

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

// Toggle: 0 silent, 1 log the resolved wiring whenever it CHANGES. A wrong
// component placement (Locomotion on the root instead of the skeleton, a mistyped
// .ianim path) is otherwise silent: the character stands in its rest pose.
#ifndef ANIMDRIVER_DEBUG
#define ANIMDRIVER_DEBUG 0
#endif

using namespace DirectX;

namespace
{
    // Self -> SkeletonRef -> depth-first animated descendant, cached on the
    // component with a generation stamp so respawns re-resolve. The walk is
    // Anim::ResolveAnimEntity, shared with AnimationClipSystem and the Lua bindings
    // so they agree on which entity holds the animation; it also stops on a cyclic
    // authored Children graph.
    Entity ResolveAnimTarget(::World& world, Entity root)
    {
        return Anim::ResolveAnimEntity(world, root);
    }

#if ANIMDRIVER_DEBUG

    // Last reported wiring per driver entity, so the log fires on change only.
    struct DriverWiring
    {
        Entity target   = NullEntity;
        int    samples  = -1;   // resolved / total, packed as resolved*100+total
        int    state    = -1;   // 0 no loco, 1 loco 1D, 2 loco 2D
    };
    std::unordered_map<Entity, DriverWiring> s_driverWiring;
#endif

    // How long an armed action may wait for ResolveAnimationSets to put it
    // on the animator before it is written off. Generous enough to cover a
    // frame or two of resolve latency, short enough that a genuinely
    // unbound clip does not freeze the character on a stale pose.
    constexpr float kActionStartTimeout = 0.25f;

    enum GaitBucket : uint8_t
    {
        kGaitNone = 0,
        kGaitIdle,
        kGaitWalkF, kGaitWalkB, kGaitWalkL, kGaitWalkR,
        kGaitRun,
        kGaitAir,
    };

    inline uint32_t XorShift(uint32_t& s)
    {
        if (s == 0) s = 0x9E3779B9u;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }

    const std::string* PickVariant(CharacterAnimDriverComponent& drv,
                                   const std::string* list, int count)
    {
        if (count <= 0) return nullptr;
        if (count == 1) return &list[0];
        if (drv.rng == 0) drv.rng = 0x9E3779B9u ^ (uint32_t)drv.animTarget;
        return &list[XorShift(drv.rng) % (uint32_t)count];
    }

    // ---- LocomotionTransitionComponent support ----
    inline float WrapDeg(float a)
    {
        a = std::fmod(a + 180.f, 360.f);
        if (a < 0.f) a += 360.f;
        return a - 180.f;
    }

    // Nearest authored spoke of the requested kind. Direction-keyed like the
    // blend space, so an 8-way set just works; a single entry authored at any
    // angle acts as the omnidirectional fallback for its kind.
    int PickTransition(const LocomotionTransitionComponent& tc,
                       LocoTransitionKind kind, float angleDeg)
    {
        int best = -1;
        float bestErr = 1e9f;
        for (int i = 0; i < tc.count &&
                        i < LocomotionTransitionComponent::MAX_TRANSITIONS; ++i)
        {
            const LocoTransition& T = tc.transitions[i];
            if (T.kind != static_cast<uint8_t>(kind)) continue;
            if (T.clip == ~0u) continue;             // not resolved yet
            const float err = std::fabs(WrapDeg(T.angleDeg - angleDeg));
            if (err < bestErr) { bestErr = err; best = i; }
        }
        return best;
    }

    void ArmTransition(LocomotionTransitionComponent& tc, int idx)
    {
        if (idx < 0) return;
        // Re-arming the loop that is already running is a no-op, not a restart.
        // A KCC on a slope or a step can flicker isGrounded across frames; each
        // flicker would otherwise snap the airborne loop back to frame 0.
        if (idx == tc.active && !tc.releasing
            && idx < tc.count && tc.transitions[idx].looping) return;
        tc.active    = idx;
        tc.time      = 0.f;
        tc.releasing = false;
        // Keep whatever weight is already faded in so a pivot chaining out of a
        // start does not pop back to zero.
        tc.weight    = std::clamp(tc.weight, 0.f, 1.f);
    }
}

namespace Character
{
// Declared here so AnimDriverTick can call it before the definition below.
void TickTransitions(::World& world, Entity e,
                     CharacterAnimDriverComponent& drv,
                     LocomotionComponent& lc, float speedXZ, float dt,
                     bool rawGrounded, float vy, float airTime);
}

namespace Character
{

void AnimDriverTick(::World& world, Resource::AnimationClipSystem& clips,
                    float dt)
{
    auto* pool = world.GetPool<CharacterAnimDriverComponent>();
    if (!pool) return;

    const auto&  entities = pool->Entities();
    auto&        drivers  = pool->Data();
    const size_t n        = drivers.size();

    for (size_t i = 0; i < n; ++i)
    {
        const Entity e = entities[i];
        CharacterAnimDriverComponent& drv = drivers[i];

        // A held action (actionHold) still gets start acknowledgement, locomotion
        // fade-out and interrupt processing; only the duration-based retirement is skipped.

        // ---- Resolve + cache the animated entity ----
        Entity target = drv.animTarget;
        if (target == NullEntity || !world.IsAlive(target)
            || world.GetGeneration(target) != drv.animTargetGen
            || !world.GetComponent<AnimationComponent>(target))
        {
            target = ResolveAnimTarget(world, e);
            drv.animTarget    = target;
            drv.animTargetGen = target != NullEntity
                              ? world.GetGeneration(target) : 0;
            drv.lastGait      = kGaitNone;
            drv.activeGaitAlias.clear();
        }
        if (target == NullEntity) continue;

        AnimationComponent* anim = world.GetComponent<AnimationComponent>(target);
        if (!anim) continue;
        if (anim->externalTime) continue;   // MotionDirector owns the clock

        // A Sequencer pose-authority claim (AnimPoseOverrideComponent) parks the driver
        // too. This system runs only in Play (RunMask::Play), so without this a shot
        // scrubs correctly in the editor and is overwritten by character state on Play.
        if (world.GetComponent<AnimPoseOverrideComponent>(target) ||
            world.GetComponent<AnimPoseOverrideComponent>(e))
            continue;

        // ---- Motor truth ----
        const CharacterControllerComponent* cc =
            world.GetComponent<CharacterControllerComponent>(e);
        // Relative to the ground: a character standing on a moving platform is not running.
        const float vx = cc ? cc->velocity.x - cc->groundVelocity.x : 0.f;
        const float vz = cc ? cc->velocity.z - cc->groundVelocity.z : 0.f;
        const float speedXZ = std::sqrt(vx * vx + vz * vz);
        const bool grounded = cc ? cc->isGrounded : true;
        const float airTime = cc ? cc->timeInAir : 0.f;

        CharacterIntentComponent* it =
            world.GetComponent<CharacterIntentComponent>(e);
        if(auto* graph=clips.GraphSystem();graph && graph->OwnsPose(target)) {
            const std::string* names[]={&drv.graphSpeedParam,&drv.graphDirectionParam,&drv.graphGroundedParam,&drv.graphVerticalSpeedParam,&drv.graphAirTimeParam,&drv.graphTurnRateParam,&drv.graphMovingParam,&drv.graphGaitParam};
            auto write=[&](size_t index,Anim::Graph::ParamType type,Anim::Graph::ParamValue value){auto& handle=drv.graphParameters[index];if(handle.name!=*names[index]){handle.name=*names[index];handle.index=Anim::Graph::kNoNode;handle.definitionHash=0;}graph->SetTyped(world,target,handle,type,value);};
            float direction=0;
            if(const auto* transform=world.GetComponent<LocalTransform>(e);transform && speedXZ>1e-3f) {
                XMFLOAT3 forward;XMStoreFloat3(&forward,XMVector3Rotate(XMVectorSet(0,0,1,0),XMLoadFloat4(&transform->rotation)));
                direction=XMConvertToDegrees(std::atan2(vx*forward.z-vz*forward.x,vx*forward.x+vz*forward.z));
            }
            float turnRate=0;
            if(const auto* transform=world.GetComponent<LocalTransform>(e)) {
                XMFLOAT3 forward;XMStoreFloat3(&forward,XMVector3Rotate(XMVectorSet(0,0,1,0),XMLoadFloat4(&transform->rotation)));
                const float yaw=XMConvertToDegrees(std::atan2(forward.x,forward.z));
                if(drv.hasPrevYaw && dt>0)turnRate=WrapDeg(yaw-drv.prevYawDeg)/dt;
                drv.prevYawDeg=yaw;drv.hasPrevYaw=true;
            }
            drv.graphMoving=speedXZ>=(drv.graphMoving?drv.walkThreshold*.5f:drv.walkThreshold);
            write(0,Anim::Graph::ParamType::Float,Anim::Graph::ParamValue::FromFloat(speedXZ));write(1,Anim::Graph::ParamType::Float,Anim::Graph::ParamValue::FromFloat(direction));
            write(2,Anim::Graph::ParamType::Bool,Anim::Graph::ParamValue::FromInt(grounded || airTime<drv.airTimeGrace));
            write(3,Anim::Graph::ParamType::Float,Anim::Graph::ParamValue::FromFloat(cc?cc->velocity.y:0));write(4,Anim::Graph::ParamType::Float,Anim::Graph::ParamValue::FromFloat(airTime));
            write(5,Anim::Graph::ParamType::Float,Anim::Graph::ParamValue::FromFloat(turnRate));write(6,Anim::Graph::ParamType::Bool,Anim::Graph::ParamValue::FromInt(drv.graphMoving));
            write(7,Anim::Graph::ParamType::Int,Anim::Graph::ParamValue::FromInt(it?static_cast<int>(it->gait):0));
            if(drv.graphAction.id) {
                const auto status=graph->SlotStatus(world,target,drv.graphAction);
                drv.actionActive=status==Anim::Graph::SlotStatus::Pending || status==Anim::Graph::SlotStatus::Playing || status==Anim::Graph::SlotStatus::BlendingOut;
                drv.actionStarted=status==Anim::Graph::SlotStatus::Playing || status==Anim::Graph::SlotStatus::BlendingOut;
            }
            if(it && it->action!=CharacterActionType::None && (!drv.actionActive || it->actionInterrupt)) {
                const std::string* alias=nullptr;const auto variant=it->actionVariant;
                if(it->action==CharacterActionType::Attack && drv.attackAliasCount>0)alias=variant>0 && variant<=drv.attackAliasCount?&drv.attackAliases[variant-1]:PickVariant(drv,drv.attackAliases,drv.attackAliasCount);
                else if(it->action==CharacterActionType::Dodge && drv.dodgeAliasCount>0)alias=variant>0 && variant<=drv.dodgeAliasCount?&drv.dodgeAliases[variant-1]:PickVariant(drv,drv.dodgeAliases,drv.dodgeAliasCount);
                else if(it->action==CharacterActionType::Interact)alias=&drv.interactAlias;
                else if(it->action==CharacterActionType::Custom)alias=&it->actionAlias;
                if(alias && !alias->empty()) {
                    Anim::Graph::SlotRequestDesc request;request.slot=drv.graphActionSlot;request.alias=*alias;request.blendIn=it->actionBlend>=0?it->actionBlend:drv.actionBlend;
                    request.interrupt=it->actionInterrupt;request.queueUntilReady=true;request.holdLastPose=it->actionHold;
                    if(auto* playback=world.GetComponent<Attack::Playback>(target);playback && playback->active && playback->alias!=*alias){playback->active=false;playback->windows=0;}
                    const auto handle=graph->PlaySlot(world,target,request);
                    if(handle.id){drv.graphAction=handle;drv.actionActive=true;drv.actionStarted=false;drv.activeActionAlias=*alias;}
                }
                it->action=CharacterActionType::None;it->actionVariant=0;it->actionAlias.clear();it->actionHold=false;it->actionBlend=-1;it->actionInterrupt=false;
            }
            continue;
        }

        // ---- Action layer (one-shot mailbox from the brain) ----
        if (drv.actionActive)
        {
            // Poll completion: the alias is neither the current nor the target clip, or
            // its one-shot playback ran out. No wall-clock duration tables.
            const std::string cur = clips.GetCurrentAnimatorClip(world, target);
            const std::string tgt = clips.GetTargetAnimatorClip(world, target);
            const bool onAnimator = (cur == drv.activeActionAlias
                                  || tgt == drv.activeActionAlias);

            bool done = false;
            if (!drv.actionStarted)
            {
                // Waiting for ResolveAnimationSets to pick up the queued play.
                // Until it does, the animator still names the OUTGOING clip, so
                // the completion test below would retire this action instantly.
                drv.actionArmTime += dt;
                if (onAnimator) drv.actionStarted = true;
                else if (drv.actionArmTime > kActionStartTimeout)
                {
                    // The alias never reached the animator: almost always an unbound clip (a
                    // .ianim still loading, or a bad path). Report it once, then drop the action.
                    ENGINE_CHECK_ONCE(false,
                        "[AnimDriver ent=%u] action '%s' never started within "
                        "%.2fs - the clip is probably still loading or its path "
                        "is wrong; dropping it.",
                        static_cast<uint32_t>(e), drv.activeActionAlias.c_str(),
                        kActionStartTimeout);
                    done = true;
                }
            }
            else
            {
                done = !onAnimator;
                if (!done && !drv.actionHold && cur == drv.activeActionAlias && !anim->looping)
                {
                    const float dur = clips.GetAnimatorDuration(world, target);
                    if (dur > 0.f && anim->primaryTime >= dur - 1e-3f)
                        done = true;
                }
            }

            if (done)
            {
                // Remember what the action left on the animator; the ramp block
                // releases it once locomotion has faded back in.
                if (drv.actionStarted) drv.parkedAlias = drv.activeActionAlias;
                drv.actionActive  = false;
                drv.actionStarted = false;
                drv.actionArmTime = 0.f;
                drv.activeActionAlias.clear();
                drv.lastGait = kGaitNone;   // force a gait re-issue
                drv.activeGaitAlias.clear();
            }
        }

        // A running action BLOCKS the mailbox rather than draining it, deliberately:
        // the request waits and fires the frame the current clip finishes, which is the
        // one-slot input buffer an attack combo chains through. `interrupt` is the
        // opt-out for requests that must cut the running action short.
        const bool haveRequest = it && it->action != CharacterActionType::None;
        if (haveRequest && (!drv.actionActive || it->actionInterrupt))
        {
            const std::string* alias = nullptr;
            bool hold = false;
            switch (it->action)
            {
                case CharacterActionType::Attack:
                {
                    if (drv.attackAliasCount > 0)
                    {
                        if (it->actionVariant > 0
                            && it->actionVariant <= drv.attackAliasCount)
                            alias = &drv.attackAliases[it->actionVariant - 1];
                        else
                            alias = PickVariant(drv, drv.attackAliases,
                                                drv.attackAliasCount);
                    }
                    break;
                }
                case CharacterActionType::Dodge:
                {
                    if (drv.dodgeAliasCount > 0)
                    {
                        if (it->actionVariant > 0
                            && it->actionVariant <= drv.dodgeAliasCount)
                            alias = &drv.dodgeAliases[it->actionVariant - 1];
                        else
                            alias = PickVariant(drv, drv.dodgeAliases,
                                                drv.dodgeAliasCount);
                    }
                    break;
                }
                case CharacterActionType::Interact:
                    if (!drv.interactAlias.empty()) alias = &drv.interactAlias;
                    break;
                case CharacterActionType::Custom:
                    if (!it->actionAlias.empty()) alias = &it->actionAlias;
                    hold = it->actionHold;
                    break;
                default:
                    break;
            }

            if (alias && !alias->empty()
                && clips.HasAnimatorClip(world, target, *alias))
            {
                const float blend = it->actionBlend >= 0.f
                                  ? it->actionBlend : drv.actionBlend;
                clips.QueueAnimatorPlay(world, target, *alias, blend,
                                        /*restart*/ true);
                if(auto* playback=world.GetComponent<Attack::Playback>(target);
                    playback&&playback->active&&playback->alias!=*alias){playback->active=false;playback->windows=0;}
                drv.actionActive      = true;
                drv.actionStarted     = false;   // not on the animator yet
                drv.actionArmTime     = 0.f;
                drv.actionHold        = hold;
                drv.activeActionAlias = *alias;
            }
            // Mailbox consumed either way so it cannot wedge. An interrupt whose alias does
            // not resolve therefore leaves the running action alone: cancelling into nothing
            // would drop the character to the blend space mid-swing.
            it->action          = CharacterActionType::None;
            it->actionVariant   = 0;
            it->actionAlias.clear();
            it->actionHold      = false;
            it->actionBlend     = -1.f;
            it->actionInterrupt = false;
        }

        // ---- Locomotion bridge ----
        // Feeds the blend space; the loco resolver does its own damping, so raw values
        // are correct. Runs even while an action plays: the parameters stay live so the
        // blend space is on the right pose when the action releases the body, and
        // locoWeight keeps ramping either way.
        LocomotionComponent* lc = world.GetComponent<LocomotionComponent>(target);
        const bool locoDrives = lc && lc->enabled && lc->sampleCount > 0;
        if (locoDrives)
        {
            lc->speed = speedXZ;
            if (lc->mode == static_cast<uint8_t>(LocomotionMode::Blend2D))
            {
                // Velocity direction in character-local space: 0 = forward,
                // +90 = right (matches LocomotionSample.angleDeg).
                if (const auto* lt = world.GetComponent<LocalTransform>(e);
                    lt && speedXZ > 1e-3f)
                {
                    const XMVECTOR q   = XMLoadFloat4(&lt->rotation);
                    const XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
                    XMFLOAT3 f; XMStoreFloat3(&f, fwd);
                    // LH: right = up x forward = (f.z, 0, -f.x)
                    const float dotF = vx * f.x + vz * f.z;
                    const float dotR = vx * f.z + vz * (-f.x);
                    lc->dirDeg = XMConvertToDegrees(std::atan2(dotR, dotF));
                }
            }

            // Hand the body to the Animator while a one-shot action runs, then fade
            // locomotion back in. AnimationSystem lerps the two poses by this weight:
            // 1 = blend space only, 0 = action clip only.
            // An action armed but not yet on the animator FREEZES the weight. Ramping toward
            // the action would uncover whatever is still parked on the animator; ramping
            // toward locomotion would blend idle into a chained combo step (it re-arms with
            // actionStarted = false). kActionStartTimeout bounds the hold.
            const bool actionPending = drv.actionActive && !drv.actionStarted;
            if (!actionPending)
            {
                float blend = drv.actionActive ? drv.actionBlend : drv.gaitBlend;
                if(const auto* attack=world.GetComponent<Attack::Playback>(target);attack&&attack->definition) {
                    if(drv.actionActive&&attack->active&&drv.activeActionAlias==attack->alias)blend=attack->definition->blendIn;
                    else if(!drv.actionActive&&drv.parkedAlias==attack->alias)blend=attack->definition->blendOut;
                }
                const float step  = (blend > 1e-3f ? dt / blend : 1.f);
                const float goal  = drv.actionActive ? 0.f : 1.f;
                lc->locoWeight += std::clamp(goal - lc->locoWeight, -step, step);
                lc->locoWeight  = std::clamp(lc->locoWeight, 0.f, 1.f);
            }

            // Release the finished one-shot once locomotion owns the body again.
            // On a blend-space character the discrete-gait branch below never runs, so
            // nothing else moves the animator off the action clip: it stays parked on its
            // final frame, hidden only by (1 - locoWeight) == 0, and every later dip in that
            // weight (the next action, a script calling Locomotion.SetWeight) smears the
            // stale pose back in. Once cleared, the next action hard-cuts in and the visible
            // fade-in is the locoWeight ramp.
            if (!drv.actionActive && lc->locoWeight >= 0.999f
                && !drv.parkedAlias.empty())
            {
                // Only the driver's OWN leftover, and only while it is still the current clip:
                // a script that started something of its own in the meantime keeps it.
                if (clips.GetCurrentAnimatorClip(world, target) == drv.parkedAlias
                    && anim->primaryClip != kInvalidAnimHandle)
                {
                    clips.StopAnimator(world, target);
                }
                drv.parkedAlias.clear();
            }

            // ---- Transition state machine (start / stop / pivot / turn) ----
            // The blend space holds a gait; this is the only thing that knows a gait was
            // ENTERED or LEFT. It only ARMS the transition: AnimationSystem owns the clock
            // and the cross-fade.
            TickTransitions(world, e, drv, *lc, speedXZ, dt,
                            grounded, cc ? cc->velocity.y : 0.f, airTime);
        }

#if ANIMDRIVER_DEBUG
        {
            int resolved = 0, total = lc ? lc->sampleCount : 0;
            if (lc)
                for (int s = 0; s < total &&
                                s < static_cast<int>(LocomotionComponent::MAX_SAMPLES); ++s)
                    if (lc->samples[s].clip != ~0u) ++resolved;

            DriverWiring now;
            now.target  = target;
            now.samples = resolved * 100 + total;
            now.state   = !locoDrives ? 0
                        : (lc->mode == static_cast<uint8_t>(LocomotionMode::Blend2D) ? 2 : 1);

            DriverWiring& prev = s_driverWiring[e];
            if (prev.target != now.target || prev.samples != now.samples
                                          || prev.state   != now.state)
            {
                prev = now;
                if (!lc)
                    LOG_WARNING("[AnimDriver ent=%u] animTarget=%u has NO "
                                "LocomotionComponent — falling back to discrete "
                                "gait aliases. The blend space must live on the "
                                "ANIMATED entity (the one with Skeleton + "
                                "Animation), not on the character root.",
                                e, static_cast<uint32_t>(target));
                else
                    LOG_INFO("[AnimDriver ent=%u] animTarget=%u loco=%s mode=%s "
                             "samples=%d/%d resolved",
                             e, static_cast<uint32_t>(target),
                             locoDrives ? "DRIVING" : "present-but-inactive",
                             now.state == 2 ? "2D" : "1D", resolved, total);
            }
        }
#endif

        if (drv.actionActive) continue;     // action owns the pose
        if (locoDrives)       continue;     // loco owns the base pose: no discrete gait

        // ---- Discrete gait fallback ----
        uint8_t gait;
        if (!grounded && airTime > drv.airTimeGrace && !drv.airAlias.empty())
            gait = kGaitAir;
        else if (speedXZ < drv.walkThreshold)
            gait = kGaitIdle;
        else if (speedXZ >= drv.runThreshold && !drv.runAlias.empty())
            gait = kGaitRun;
        else
        {
            // Walk band: classify against facing when directional aliases exist
            // (+-60 deg forward/back cones, sides by right-axis sign).
            gait = kGaitWalkF;
            const bool hasDirectional = !drv.walkBackAlias.empty()
                                     || !drv.walkLeftAlias.empty()
                                     || !drv.walkRightAlias.empty();
            if (hasDirectional)
            {
                if (const auto* lt = world.GetComponent<LocalTransform>(e))
                {
                    const XMVECTOR q   = XMLoadFloat4(&lt->rotation);
                    const XMVECTOR fwdV = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
                    XMFLOAT3 f; XMStoreFloat3(&f, fwdV);
                    const float fLen = std::sqrt(f.x * f.x + f.z * f.z);
                    if (fLen > 1e-3f)
                    {
                        const float fx = f.x / fLen, fz = f.z / fLen;
                        const float mx = vx / speedXZ, mz = vz / speedXZ;
                        const float forwardDot = fx * mx + fz * mz;
                        // Right axis in LH XZ: (fz, -fx).
                        const float rightDot   = fz * mx - fx * mz;
                        constexpr float kCos60 = 0.5f;
                        if (forwardDot < -kCos60 && !drv.walkBackAlias.empty())
                            gait = kGaitWalkB;
                        else if (forwardDot <= kCos60)
                        {
                            if (rightDot >= 0.f && !drv.walkRightAlias.empty())
                                gait = kGaitWalkR;
                            else if (rightDot < 0.f && !drv.walkLeftAlias.empty())
                                gait = kGaitWalkL;
                        }
                    }
                }
            }
        }

        // Idle variant re-roll timer.
        bool reroll = false;
        if (gait == kGaitIdle && drv.lastGait == kGaitIdle
            && drv.idleRerollSec > 0.f)
        {
            drv.idleTimer += dt;
            if (drv.idleTimer >= drv.idleRerollSec)
            {
                drv.idleTimer = 0.f;
                reroll = true;
            }
        }
        else if (gait == kGaitIdle && drv.lastGait != kGaitIdle)
        {
            drv.idleTimer = 0.f;
        }

        if (gait == drv.lastGait && !reroll) continue;

        const std::string* alias = nullptr;
        switch (gait)
        {
            case kGaitIdle:
                alias = PickVariant(drv, drv.idleAliases, drv.idleAliasCount);
                break;
            case kGaitWalkF:
                alias = PickVariant(drv, drv.walkAliases, drv.walkAliasCount);
                break;
            case kGaitWalkB: alias = &drv.walkBackAlias;  break;
            case kGaitWalkL: alias = &drv.walkLeftAlias;  break;
            case kGaitWalkR: alias = &drv.walkRightAlias; break;
            case kGaitRun:   alias = &drv.runAlias;       break;
            case kGaitAir:   alias = &drv.airAlias;       break;
            default: break;
        }
        if (!alias || alias->empty()) { drv.lastGait = gait; continue; }
        if (!clips.HasAnimatorClip(world, target, *alias)) continue; // not registered yet: retry next tick

        float gaitBlend=drv.gaitBlend;
        if(const auto* attack=world.GetComponent<Attack::Playback>(target);
            attack&&attack->definition&&drv.parkedAlias==attack->alias)gaitBlend=attack->definition->blendOut;
        clips.QueueAnimatorPlay(world, target, *alias, gaitBlend,
                                /*restart*/ false);
        drv.lastGait        = gait;
        drv.activeGaitAlias = *alias;
    }
}

// TickTransitions: the gait entry/exit state machine the blend space lacks.
// Detects these events from motor truth and arms the matching clip. It never
// touches the clock or the weight (AnimationSystem owns those), so a paused frame
// freezes a transition like any other clip.
//   Start       speed crosses startSpeed upward   -> spoke = new direction
//   Stop        speed falls below stopSpeed       -> spoke = last direction
//   Pivot       direction jumps > pivotAngle      -> spoke = new direction
//   TurnInPlace yaw rate > turnRate while stopped -> sign = turn direction
// plus the airborne JumpStart / JumpLoop / JumpEnd sequence. On the ground only a
// Pivot interrupts a running transition; every other event waits.
void TickTransitions(::World& world, Entity e,
                     CharacterAnimDriverComponent& drv,
                     LocomotionComponent& lc, float speedXZ, float dt,
                     bool rawGrounded, float vy, float airTime)
{
    // A real jump (upward velocity) enters the air state at once. A loss of ground
    // without upward velocity must outlast airTimeGrace first: stepping off a kerb or
    // down a stair un-grounds the KCC for a frame or two, and a fall-and-land
    // sequence for that reads as a twitch. Same grace as the discrete gait path.
    const bool airborne = !rawGrounded
                       && (vy > 0.1f || airTime >= drv.airTimeGrace);
    const bool grounded = !airborne;
    auto* tc = world.GetComponent<LocomotionTransitionComponent>(drv.animTarget);

    // Facing yaw comes from the character ROOT (the entity whose LocalTransform is
    // the dirDeg basis), not from the animated skeleton entity.
    float yawDeg = drv.prevYawDeg;
    bool  yawOK  = false;
    if (const auto* lt = world.GetComponent<LocalTransform>(e))
    {
        const XMVECTOR q   = XMLoadFloat4(&lt->rotation);
        const XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
        XMFLOAT3 f; XMStoreFloat3(&f, fwd);
        yawDeg = XMConvertToDegrees(std::atan2(f.x, f.z));
        yawOK  = true;
    }

    const bool noMachine = !tc || !tc->enabled || tc->count == 0;
    if (noMachine || dt <= 0.f)
    {
        // Still track history so the first frame after enabling doesn't fire a
        // bogus event from a stale delta.
        drv.prevSpeed  = speedXZ;
        drv.prevDirDeg = lc.dirDeg;
        if (yawOK) { drv.prevYawDeg = yawDeg; drv.hasPrevYaw = true; }
        // A frozen frame (hit-stop, timeScale 0, editor pause) must not consume the
        // ground/air edge, or a landing on that frame is lost and the airborne loop keeps
        // playing on the ground. Only a missing state machine drops the edge.
        if (noMachine) drv.wasGrounded = grounded;
        else if (!grounded && tc) tc->loopHeld = true;   // keep the loop alive
        return;
    }

    // ---- Airborne state machine ----
    // Keyed on the KCC grounded flag, never on the jump input: walking off a ledge
    // falls exactly like a jump. The vertical velocity at the instant ground is lost
    // picks the first clip, so a real jump gets its wind-up and a plain fall drops
    // straight into the loop.
    // Runs before (and returns instead of) the ground events: a stop or pivot in
    // mid-air would fight the fall.
    const int   activeKind = (tc->active >= 0 && tc->active < tc->count)
                           ? static_cast<int>(tc->transitions[tc->active].kind) : -1;
    const auto  kJumpStart = static_cast<int>(LocoTransitionKind::JumpStart);
    const auto  kJumpLoop  = static_cast<int>(LocoTransitionKind::JumpLoop);

    if (!grounded)
    {
        // Heartbeat first, unconditionally: whatever is decided below, being airborne
        // is what keeps a held loop alive.
        tc->loopHeld = true;

        if (drv.wasGrounded)
        {
            // Ground -> air this frame.
            int p = (vy > 0.1f)
                  ? PickTransition(*tc, LocoTransitionKind::JumpStart, 0.f) : -1;
            if (p < 0) p = PickTransition(*tc, LocoTransitionKind::JumpLoop, 0.f);
            ArmTransition(*tc, p);
        }
        else if (activeKind != kJumpLoop
                 && (tc->active < 0
                     || (activeKind == kJumpStart && tc->releasing)))
        {
            // Chain into the airborne loop only once the wind-up is releasing:
            // `activeKind == kJumpStart` alone is already true the frame after arming it and
            // would cut JumpStart after one frame. Waiting for `releasing` also cross-fades
            // the loop in as JumpStart fades out, with no gap through the blend-space pose.
            const int p = PickTransition(*tc, LocoTransitionKind::JumpLoop, 0.f);
            if (p >= 0 && p != tc->active) ArmTransition(*tc, p);
        }

        drv.prevSpeed   = speedXZ;
        drv.prevDirDeg  = lc.dirDeg;
        drv.wasGrounded = grounded;
        if (yawOK) { drv.prevYawDeg = yawDeg; drv.hasPrevYaw = true; }
        return;
    }

    if (!drv.wasGrounded)
    {
        // Air -> ground: land. Not re-asserting loopHeld is what releases the
        // airborne loop; arming JumpEnd on top of it is the visible landing.
        const int p = PickTransition(*tc, LocoTransitionKind::JumpEnd, 0.f);
        if (p >= 0) ArmTransition(*tc, p);

        // Do not overwrite prevSpeed / prevDirDeg here: this is the first ground frame,
        // and landing while already holding a direction is a genuine Start edge that
        // overwriting would consume (landing into a walk with no step-off). Pivot needs
        // isMoving && wasMoving, so stale air values cannot fake one.
        drv.wasGrounded = grounded;
        if (yawOK) { drv.prevYawDeg = yawDeg; drv.hasPrevYaw = true; }
        return;
    }
    drv.wasGrounded = grounded;

    const float prevSpeed = drv.prevSpeed;
    const float prevDir   = drv.prevDirDeg;
    const bool  wasMoving = prevSpeed >= tc->startSpeed;
    const bool  isMoving  = speedXZ   >= tc->startSpeed;
    const bool  stopped   = speedXZ   <  tc->stopSpeed;

    const bool  busy      = tc->active >= 0 && !tc->releasing;
    const bool  cooling   = tc->cooldownTimer > 0.f;

    int pick = -1;

    // 1) PIVOT - highest priority: it can interrupt a running transition.
    if (isMoving && wasMoving)
    {
        const float turned = std::fabs(WrapDeg(lc.dirDeg - prevDir));
        if (turned >= tc->pivotAngle)
            pick = PickTransition(*tc, LocoTransitionKind::Pivot, lc.dirDeg);
        if (pick >= 0) ArmTransition(*tc, pick);
    }

    if (pick < 0 && !busy && !cooling)
    {
        // 2) START - idle -> moving.
        if (isMoving && !wasMoving)
            pick = PickTransition(*tc, LocoTransitionKind::Start, lc.dirDeg);
        // 3) STOP - moving -> idle. Uses the LAST movement direction, because
        //    dirDeg is frozen once velocity drops below the driver's epsilon.
        else if (stopped && wasMoving)
            pick = PickTransition(*tc, LocoTransitionKind::Stop, prevDir);
        // 4) TURN IN PLACE - stationary but yawing hard.
        else if (stopped && drv.hasPrevYaw && yawOK)
        {
            const float yawRate = WrapDeg(yawDeg - drv.prevYawDeg) / dt;
            if (std::fabs(yawRate) >= tc->turnRate)
                pick = PickTransition(*tc, LocoTransitionKind::TurnInPlace,
                                      yawRate >= 0.f ? 90.f : -90.f);
        }
        if (pick >= 0) ArmTransition(*tc, pick);
    }

    drv.prevSpeed  = speedXZ;
    drv.prevDirDeg = lc.dirDeg;
    if (yawOK) { drv.prevYawDeg = yawDeg; drv.hasPrevYaw = true; }
}

// CancelAction (contract at the declaration). A cancelled action leaves its
// clip on the animator like a completed one, so it must hand over parkedAlias
// the same way: otherwise the clip stays parked forever and the next action
// cross-fades out of its stale final pose for a frame or two.
bool CancelAction(::World& world, Entity e)
{
    if(auto* p=world.GetComponent<Attack::Playback>(Anim::ResolveAnimEntityOrSelf(world,e))){p->active=false;p->windows=0;}
    auto* drv = world.GetComponent<CharacterAnimDriverComponent>(e);
    if (!drv || !drv->actionActive) return false;
    if(drv->graphAction.id)if(auto* parameters=world.GetComponent<AnimGraphParamsComponent>(Anim::ResolveAnimEntity(world,e))) {
        Anim::Graph::SlotRequestDesc stop;stop.blendOut=drv->actionBlend;parameters->slotRequests.push_back({stop,drv->graphAction,true});drv->graphAction={};
    }
    // Only if it actually reached the animator; an action cancelled before it
    // started has nothing parked.
    if (drv->actionStarted) drv->parkedAlias = drv->activeActionAlias;
    drv->actionActive  = false;
    drv->actionStarted = false;
    drv->actionArmTime = 0.f;
    drv->actionHold    = false;
    drv->activeActionAlias.clear();
    drv->lastGait = kGaitNone;          // force a gait re-issue next tick
    drv->activeGaitAlias.clear();
    return true;
}

} // namespace Character
