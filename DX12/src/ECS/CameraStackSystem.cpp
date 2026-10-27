// CameraStackSystem.cpp - the virtual-camera stack pipeline
// (docs/design/camera_stack_system.md, include/ECS/CameraStackComponents.h).

#include "ECS/CameraStackSystem.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/CameraMotionComponent.h"        // CameraMotionTickSystem
#include "ECS/MotionDirectorComponent.h"      // MotionDirectorTickSystem
#include "ECS/AnimationComponents.h"          // skinned-track AnimationComponent
#include "Audio/AudioComponents.h"            // audio-track AudioSourceComponent
#include "ECS/FrameContext.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"  // GlobalTransform
#include "ECS/CharacterControllerComponent.h" // aim target's own query body
#include "ECS/PhysicsComponents.h"            // RigidBodyComponent::bodyId
#include "ECS/ExclusiveResourceIds.h" // kPhysicsWorld (Aim spherecast)
#include "ECS/ECS.h"
#include "System/Log.h"
#include "Physics/PhysicsSystem.h"
#include "Resource/AnimationClipSystem.h"     // camera clip acquire/sample
#include "Resource/SkeletonAsset.h"           // ClipLibrary (skinned-clip durations)

#include <DirectXMath.h>
#include <algorithm>
#include <atomic>
#include <cmath>

using namespace DirectX;

namespace
{
    // The aim target's OWN physics body, excluded from the wall-collision
    // probe so the arm cannot collide with the character it orbits. See the
    // matching note in CameraSystem::ResolveFollowing: with
    // CharacterControllerComponent::queryBody the pivot sits inside the
    // target's own capsule and an unfiltered sweep pins the camera at the
    // minimum distance. kInvalidPhysicsBodyId means "filter nothing".
    std::uint32_t AimTargetOwnBody(World& world, Entity e)
    {
        if (const auto* cc = world.GetComponent<CharacterControllerComponent>(e))
            if (cc->bodyId != kInvalidPhysicsBodyId) return cc->bodyId;
        if (const auto* rb = world.GetComponent<RigidBodyComponent>(e))
            if (rb->bodyId != kInvalidPhysicsBodyId) return rb->bodyId;
        return kInvalidPhysicsBodyId;
    }

    // ---- Math helpers ----

    inline XMVECTOR LoadPos(const CameraPoseComponent& p)
    {
        return XMLoadFloat3(&p.position);
    }
    inline XMVECTOR LoadRot(const CameraPoseComponent& p)
    {
        return XMQuaternionNormalize(XMLoadFloat4(&p.rotation));
    }

    // Shortest-path nlerp q1 -> q2 by t: q2 is negated when dot < 0 so the blend goes
    // the short way.
    XMVECTOR NLerpShortest(XMVECTOR q1, XMVECTOR q2, float t)
    {
        const float d = XMVectorGetX(XMVector4Dot(q1, q2));
        if (d < 0.f) q2 = XMVectorNegate(q2);
        XMVECTOR r = XMVectorAdd(
            XMVectorScale(q1, 1.f - t),
            XMVectorScale(q2, t));
        return XMQuaternionNormalize(r);
    }

    // Blend tan(fov / 2): perceptually uniform zoom interpolation.
    float BlendFovTan(float fovA, float fovB, float t)
    {
        const float ta = std::tan(0.5f * fovA);
        const float tb = std::tan(0.5f * fovB);
        const float tc = ta * (1.f - t) + tb * t;
        return 2.f * std::atan(tc);
    }

    // Forward vector of a rotation quaternion: (0,0,1) rotated by q, the same axis as
    // GlobalTransform.matrix row 2 (engine convention).
    XMVECTOR ForwardFromQuat(XMVECTOR q)
    {
        return XMVector3Normalize(XMVector3Rotate(XMVectorSet(0.f, 0.f, 1.f, 0.f), q));
    }

    // Backfill the four core VCam components onto an entity; idempotent, present
    // components are left alone.
    void EnsureVCamComponents(World& world, Entity e)
    {
        if (!world.HasComponent<VirtualCameraComponent>(e))
            world.AddComponent<VirtualCameraComponent>(e, VirtualCameraComponent{});
        if (!world.HasComponent<CameraPoseComponent>(e))
            world.AddComponent<CameraPoseComponent>(e, CameraPoseComponent{});
        if (!world.HasComponent<VCamPriorityComponent>(e))
            world.AddComponent<VCamPriorityComponent>(e, VCamPriorityComponent{});
        if (!world.HasComponent<VCamBlendComponent>(e))
            world.AddComponent<VCamBlendComponent>(e, VCamBlendComponent{});
    }
}

namespace Camera
{
    // ---- Channel-entity bookkeeping ----

    Entity FindChannelEntity(World& world, CameraChannelId channelId)
    {
        Entity found = NullEntity;
        world.ForEach<CameraStackChannelSingleton>(
            [&](Entity e, CameraStackChannelSingleton& s) {
                if (found == NullEntity && s.channelId == channelId) found = e;
            });
        return found;
    }

    Entity GetOrCreateChannelEntity(World& world, CameraChannelId channelId)
    {
        if (Entity existing = FindChannelEntity(world, channelId);
            existing != NullEntity)
        {
            // Channel entities are runtime bookkeeping: backfill the SceneTransientTag here as
            // well as at creation, so a channel entity made without it (e.g. by a hot-reloaded
            // build) never leaks into a saved scene.
            if (!world.HasComponent<SceneTransientTag>(existing))
                world.AddComponent<SceneTransientTag>(existing, SceneTransientTag{});
            return existing;
        }
        const Entity e = world.CreateEntity();
        world.AddComponent<SceneTransientTag>(e, SceneTransientTag{});
        CameraStackChannelSingleton s{};
        s.channelId = channelId;
        world.AddComponent<CameraStackChannelSingleton>(e, std::move(s));
        LiveCameraComponent live{};
        live.channelId = channelId;
        world.AddComponent<LiveCameraComponent>(e, std::move(live));
        return e;
    }

    void PushVCam(World& world, Entity vcam, const PushVCamArgs& args)
    {
        if (!world.IsAlive(vcam))
        {
            LOG_WARNING("Camera.PushVCam: entity %u not alive", vcam);
            return;
        }
        EnsureVCamComponents(world, vcam);

        if (auto* vc = world.GetComponent<VirtualCameraComponent>(vcam))
            vc->channelId = args.channelId;

        auto* prio = world.GetComponent<VCamPriorityComponent>(vcam);
        if (prio) {
            prio->priority = args.priority;
            prio->weight   = args.weight;
            prio->enabled  = true;
        }

        auto* blend = world.GetComponent<VCamBlendComponent>(vcam);
        if (blend) {
            blend->blendInDuration  = args.blendInDuration;
            blend->blendOutDuration = args.blendOutDuration;
            blend->curveIn          = args.curveIn;
            blend->curveOut         = args.curveOut;
            // Transition to BlendingIn from the CURRENT blend value: a re-push during
            // BlendingOut must not snap back to 0.
            if (blend->state == BlendState::Inactive ||
                blend->state == BlendState::BlendingOut)
            {
                blend->state = BlendState::BlendingIn;
            }
            // If zero-duration blendIn, jump straight to Active.
            if (args.blendInDuration <= 0.f) {
                blend->currentBlend = 1.f;
                blend->state        = BlendState::Active;
            }
        }
        GetOrCreateChannelEntity(world, args.channelId);
    }

    void PopVCam(World& world, Entity vcam,
                 float blendOutOverride,
                 BlendCurve curveOverride)
    {
        if (!world.IsAlive(vcam)) return;
        auto* prio  = world.GetComponent<VCamPriorityComponent>(vcam);
        auto* blend = world.GetComponent<VCamBlendComponent>(vcam);
        if (!prio || !blend) return;

        if (blendOutOverride >= 0.f) blend->blendOutDuration = blendOutOverride;
        // Sentinel: Custom passed by default in the public API means "leave alone".
        if (curveOverride != BlendCurve::Custom) blend->curveOut = curveOverride;

        // Mark disabled so the stack stops electing it as winner; the stack
        // tick will transition us to BlendingOut and keep contributing until
        // currentBlend reaches 0.
        prio->enabled = false;
        if (blend->blendOutDuration <= 0.f) {
            blend->currentBlend = 0.f;
            blend->state        = BlendState::Inactive;
        }
    }

    void HardCutTo(World& world, Entity vcam, CameraChannelId channelId)
    {
        const Entity ch = GetOrCreateChannelEntity(world, channelId);
        if (auto* s = world.GetComponent<CameraStackChannelSingleton>(ch))
            s->requestHardCut = true;

        if (vcam != NullEntity)
        {
            PushVCamArgs a{};
            a.priority         = 10000;   // arbitrary high
            a.blendInDuration  = 0.f;
            a.blendOutDuration = 0.f;
            a.channelId        = channelId;
            PushVCam(world, vcam, a);
        }
    }

    namespace
    {
        std::atomic<float> g_shakeScale{ 1.f };   // SetShakeScale
    }

    void SetShakeScale(float scale)
    {
        g_shakeScale.store(std::clamp(scale, 0.f, 1.f), std::memory_order_relaxed);
    }

    float ShakeScale()
    {
        return g_shakeScale.load(std::memory_order_relaxed);
    }
}

// ==== Behavior systems ====

void FollowCameraTickSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    world.ForEach<FollowCameraTag>([&](Entity e, FollowCameraTag&)
    {
        auto* fc   = world.GetComponent<FollowCameraComponent>(e);
        auto* pose = world.GetComponent<CameraPoseComponent>(e);
        if (!fc || !pose) return;
        const Entity targetE = fc->target.Resolve(world);
        if (targetE == NullEntity) return;

        auto* targetGT = world.GetComponent<GlobalTransform>(targetE);
        if (!targetGT) return;

        const XMMATRIX M = XMLoadFloat4x4(&targetGT->matrix);
        const XMVECTOR targetPos = M.r[3];
        // Use the target's rotation so the offset feels "behind/above target".
        XMVECTOR targetRot;
        XMVECTOR scale, trans;
        XMMatrixDecompose(&scale, &targetRot, &trans, M);

        // Desired world-space camera position = target + targetRot * offset.
        const XMVECTOR desired = XMVectorAdd(
            targetPos,
            XMVector3Rotate(XMLoadFloat3(&fc->offset), targetRot));

        // Look-at (optional).
        XMVECTOR desiredRot;
        if (fc->useLookAt)
        {
            const XMVECTOR lookAtPt = XMVectorAdd(
                targetPos,
                XMVector3Rotate(XMLoadFloat3(&fc->lookAtOffset), targetRot));
            const XMVECTOR fwd = XMVector3Normalize(
                XMVectorSubtract(lookAtPt, desired));
            XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
            // Avoid the lookAt singularity when forward ~ up.
            if (std::abs(XMVectorGetX(XMVector3Dot(fwd, up))) > 0.999f)
                up = XMVectorSet(0.f, 0.f, 1.f, 0.f);
            // Build a quaternion that rotates +Z to fwd.
            const XMMATRIX look = XMMatrixLookToLH(XMVectorZero(), fwd, up);
            // LookTo gives a view matrix; world rotation is its transpose.
            const XMMATRIX worldRot = XMMatrixTranspose(look);
            desiredRot = XMQuaternionRotationMatrix(worldRot);
        }
        else
        {
            desiredRot = targetRot;
        }

        // Fixed per-frame smoothing toward the desired pose, tuned for 60 Hz: dt and
        // FollowCameraComponent::damping / rotationDamping are not used.
        const float kPosLerp = 0.25f;
        const float kRotLerp = 0.25f;
        XMVECTOR cur = XMLoadFloat3(&pose->position);
        cur = XMVectorLerp(cur, desired, kPosLerp);
        XMStoreFloat3(&pose->position, cur);

        XMVECTOR curRot = XMQuaternionNormalize(XMLoadFloat4(&pose->rotation));
        curRot = NLerpShortest(curRot, desiredRot, kRotLerp);
        XMStoreFloat4(&pose->rotation, curRot);
    });
}

void AimCameraTickSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    world.ForEach<AimCameraTag>([&](Entity e, AimCameraTag&)
    {
        auto* aim  = world.GetComponent<AimCameraComponent>(e);
        auto* pose = world.GetComponent<CameraPoseComponent>(e);
        if (!aim || !pose) return;
        const Entity targetE = aim->target.Resolve(world);
        if (targetE == NullEntity) return;

        // Clamp pitch.
        aim->pitch = std::clamp(aim->pitch, aim->pitchMin, aim->pitchMax);

        auto* targetGT = world.GetComponent<GlobalTransform>(targetE);
        if (!targetGT) return;
        const XMMATRIX M = XMLoadFloat4x4(&targetGT->matrix);
        const XMVECTOR targetPos = M.r[3];

        const XMVECTOR pivot = XMVectorAdd(
            targetPos, XMLoadFloat3(&aim->pivotOffset));

        // Camera rotation built from yaw/pitch (world Y, then camera X).
        const XMVECTOR qYaw   = XMQuaternionRotationAxis(
            XMVectorSet(0.f, 1.f, 0.f, 0.f), aim->yaw);
        const XMVECTOR right  = XMVector3Rotate(
            XMVectorSet(1.f, 0.f, 0.f, 0.f), qYaw);
        const XMVECTOR qPitch = XMQuaternionRotationAxis(right, aim->pitch);
        const XMVECTOR qRot   = XMQuaternionMultiply(qYaw, qPitch);

        // Forward of the resulting basis points +Z (engine convention).
        const XMVECTOR forward = ForwardFromQuat(qRot);

        float dist = aim->distance;
        if (m_physics && aim->collisionAvoid && dist > 0.f)
        {
            XMFLOAT3 from, dir;
            XMStoreFloat3(&from, pivot);
            XMStoreFloat3(&dir,  XMVectorNegate(forward));
            const auto hit = m_physics->CastSphereClosest(
                from, dir, std::max(0.01f, aim->probeRadius), dist,
                AimTargetOwnBody(world, targetE),
                DX12Physics::LAYER_MASK_CAMERA_PROBE);
            if (hit.hit) dist = std::max(0.05f, hit.distance);
        }
        const XMVECTOR camPos = XMVectorSubtract(
            pivot, XMVectorScale(forward, dist));

        XMStoreFloat3(&pose->position, camPos);
        XMStoreFloat4(&pose->rotation, qRot);
    });
}

// ==== Stack + Resolve + Shake (PreRender) ====

void CameraStackTickSystem::Update(World& world, const FrameContext& ctx)
{
    const float dt = std::max(ctx.deltaTime, 0.f);

    // Bridge: a VCam without a behavior tag mirrors its pose from GlobalTransform.
    // That is how the FPS camera (CameraSystem -> LocalTransform -> TransformSystem ->
    // GlobalTransform) joins the stack without writing CameraPoseComponent itself.
    world.ForEach<VirtualCameraComponent>(
        [&](Entity e, VirtualCameraComponent&)
    {
        if (world.HasComponent<FollowCameraTag>(e)) return;
        if (world.HasComponent<AimCameraTag>(e))    return;
        // CameraMotion drives its own CameraPoseComponent from a camera .ianim: never
        // bridge it from the entity's (static) GlobalTransform, or the bridge clobbers the
        // animated position / rotation every frame and only the FOV moves.
        if (world.HasComponent<CameraMotionComponent>(e)) return;
        auto* pose = world.GetComponent<CameraPoseComponent>(e);
        auto* gt   = world.GetComponent<GlobalTransform>(e);
        if (!pose || !gt) return;
        const XMMATRIX M = XMLoadFloat4x4(&gt->matrix);
        XMVECTOR s, r, t;
        XMMatrixDecompose(&s, &r, &t, M);
        XMStoreFloat3(&pose->position, t);
        XMStoreFloat4(&pose->rotation, XMQuaternionNormalize(r));
    });

    // For each channel: rebuild entries, identify winning priority bucket,
    // advance every contributing VCam's blend state machine.
    world.ForEach<CameraStackChannelSingleton>(
        [&](Entity /*channelEnt*/, CameraStackChannelSingleton& chan)
    {
        chan.entries.clear();

        // Gather VCams targeting this channel.
        world.ForEach<VirtualCameraComponent>(
            [&](Entity vcam, VirtualCameraComponent& vc)
        {
            if (vc.channelId != chan.channelId) return;
            auto* prio  = world.GetComponent<VCamPriorityComponent>(vcam);
            auto* blend = world.GetComponent<VCamBlendComponent>(vcam);
            if (!prio || !blend) return;
            // Include if enabled OR still blending out (still contributing).
            const bool contributing = (prio->enabled)
                                    || (blend->currentBlend > 0.f)
                                    || (blend->state == BlendState::BlendingOut);
            if (!contributing) return;

            CameraStackChannelSingleton::StackEntry e{};
            e.vcam         = vcam;
            e.priority     = prio->priority;
            e.weight       = prio->weight;
            e.currentBlend = blend->currentBlend;
            chan.entries.push_back(e);
        });

        // Sort priority desc, ties by entity id for stability.
        std::sort(chan.entries.begin(), chan.entries.end(),
            [](const auto& a, const auto& b) {
                if (a.priority != b.priority) return a.priority > b.priority;
                return a.vcam < b.vcam;
            });

        // Determine the winning priority bucket. The bucket is "the highest
        // priority bucket containing at least one enabled VCam". If no VCam
        // is enabled (all popped), the BlendingOut survivors get to blend
        // down naturally and no one is a winner.
        int winningPriority = INT_MIN;
        for (const auto& e : chan.entries)
        {
            auto* prio = world.GetComponent<VCamPriorityComponent>(e.vcam);
            if (prio && prio->enabled) { winningPriority = e.priority; break; }
        }

        // Advance every contributing VCam.
        for (auto& e : chan.entries)
        {
            auto* prio  = world.GetComponent<VCamPriorityComponent>(e.vcam);
            auto* blend = world.GetComponent<VCamBlendComponent>(e.vcam);
            if (!prio || !blend) continue;

            // A Sequencer-owned VCam has already had its blend written for this
            // frame as a pure function of the playhead. Integrating dt on top
            // would reintroduce exactly the path-dependence that makes a
            // cinematic un-scrubbable. Publish, don't advance.
            if (world.HasComponent<SeqCameraOwnedTag>(e.vcam))
            {
                e.currentBlend = blend->currentBlend;
                continue;
            }

            const bool isWinner = prio->enabled && e.priority == winningPriority;

            if (isWinner)
            {
                if (blend->state == BlendState::Inactive ||
                    blend->state == BlendState::BlendingOut)
                {
                    blend->state = BlendState::BlendingIn;
                }
                if (blend->state == BlendState::BlendingIn)
                {
                    if (blend->blendInDuration <= 0.f) {
                        blend->currentBlend = 1.f;
                    } else {
                        blend->currentBlend += dt / blend->blendInDuration;
                    }
                    if (blend->currentBlend >= 1.f) {
                        blend->currentBlend = 1.f;
                        blend->state = BlendState::Active;
                    }
                }
                else // Active
                {
                    blend->currentBlend = 1.f;
                }
            }
            else // loser this frame
            {
                if (blend->state == BlendState::Active ||
                    blend->state == BlendState::BlendingIn)
                {
                    blend->state = BlendState::BlendingOut;
                }
                if (blend->state == BlendState::BlendingOut)
                {
                    if (blend->blendOutDuration <= 0.f) {
                        blend->currentBlend = 0.f;
                    } else {
                        blend->currentBlend -= dt / blend->blendOutDuration;
                    }
                    if (blend->currentBlend <= 0.f) {
                        blend->currentBlend = 0.f;
                        blend->state = BlendState::Inactive;
                    }
                }
            }
            e.currentBlend = blend->currentBlend;
        }
    });
}

void CameraResolveTickSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    world.ForEach<LiveCameraComponent>([&](Entity channelEnt, LiveCameraComponent& live)
    {
        auto* chan = world.GetComponent<CameraStackChannelSingleton>(channelEnt);
        if (!chan) return;

        // Filter contributing entries (currentBlend > 0).
        struct Contrib { Entity vcam; float w; };
        std::vector<Contrib> contribs;
        contribs.reserve(chan->entries.size());

        // Apply curve only at resolve time so the stored currentBlend remains
        // the linear progress (easier to reason about + author-friendly).
        float sumW = 0.f;
        for (const auto& e : chan->entries)
        {
            if (e.currentBlend <= 0.f) continue;
            auto* bc = world.GetComponent<VCamBlendComponent>(e.vcam);
            const BlendCurve curve = bc
                ? (bc->state == BlendState::BlendingOut ? bc->curveOut : bc->curveIn)
                : BlendCurve::Linear;
            const float curved = Camera::ApplyBlendCurve(curve, e.currentBlend);
            const float w = curved * e.weight;
            if (w <= 0.f) continue;
            contribs.push_back({ e.vcam, w });
            sumW += w;
        }

        if (sumW <= 1e-6f)
        {
            // Nobody contributing: hold last valid pose. historyValid stays
            // whatever it was; nothing new to render-from anyway.
            return;
        }

        // Normalize.
        const float invSum = 1.f / sumW;
        for (auto& c : contribs) c.w *= invSum;

        // Blend the pose: position, tan(fov / 2), near and far are normalized weighted
        // sums; the rotation is seeded by the first contributor and nlerped toward each
        // later one (shortest path, never a blind quaternion sum).
        XMVECTOR accPos = XMVectorZero();
        XMVECTOR accRot = XMVectorZero();
        float    accFovTan = 0.f;
        float    accNear   = 0.f;
        float    accFar    = 0.f;
        bool     first = true;
        for (const auto& c : contribs)
        {
            auto* pose = world.GetComponent<CameraPoseComponent>(c.vcam);
            auto* vc   = world.GetComponent<VirtualCameraComponent>(c.vcam);
            if (!pose || !vc) continue;

            const XMVECTOR p = LoadPos(*pose);
            const XMVECTOR q = LoadRot(*pose);
            const float    t = std::tan(0.5f * vc->fov);

            if (first)
            {
                accPos = XMVectorScale(p, c.w);
                accRot = q;             // seed
                accFovTan = t * c.w;
                accNear   = vc->nearZ * c.w;
                accFar    = vc->farZ  * c.w;
                first = false;
            }
            else
            {
                accPos = XMVectorAdd(accPos, XMVectorScale(p, c.w));
                accFovTan += t * c.w;
                accNear   += vc->nearZ * c.w;
                accFar    += vc->farZ  * c.w;
                // nlerp by c.w directly rather than by w_i / running sum: exact for two
                // contributors (the weights sum to 1), a tiny visual error beyond that.
                accRot = NLerpShortest(accRot, q, c.w);
            }
        }

        // historyValid semantics: true iff (a) we wrote a valid pose at least
        // one frame ago AND (b) no hard cut was requested this frame. The
        // first-ever frame has hasPrev=false so historyValid stays false;
        // the second frame flips it true. HardCutTo() forces one more
        // historyValid=false frame even if hasPrev is true.
        const bool hadValidPrev = live.hasPrev;
        const bool hardCut      = chan->requestHardCut;
        chan->requestHardCut    = false;

        XMStoreFloat3(&live.position, accPos);
        XMVECTOR rot = XMQuaternionNormalize(accRot);
        XMStoreFloat4(&live.rotation, rot);
        XMStoreFloat3(&live.forward, ForwardFromQuat(rot));
        live.fov   = 2.f * std::atan(accFovTan);
        live.nearZ = accNear;
        live.farZ  = accFar;

        live.historyValid = hadValidPrev && !hardCut;
        live.hasPrev      = true;
    });
}

void CameraShakeTickSystem::Update(World& world, const FrameContext& ctx)
{
    const float dt = std::max(ctx.deltaTime, 0.f);

    // Shake lives on the LiveCamera entity (per channel): add trauma^2-scaled noise on
    // top of the resolved pose, then decay trauma.
    world.ForEach<CameraShakeComponent>(
        [&](Entity e, CameraShakeComponent& shake)
    {
        if (shake.trauma <= 0.f) return;
        auto* live = world.GetComponent<LiveCameraComponent>(e);
        if (!live) return;

        shake.time += dt * shake.frequency;
        const float strength = shake.trauma * shake.trauma * Camera::ShakeScale();

        // Cheap deterministic noise from sine combos, a fixed phase offset per axis
        // (shake.seed is not used).
        auto noise = [&](float phase) {
            return std::sin(shake.time * 1.7f + phase) * 0.5f
                 + std::sin(shake.time * 2.3f + phase * 1.7f) * 0.5f;
        };
        const float nx = noise(0.f);
        const float ny = noise(1.3f);
        const float nz = noise(2.7f);

        XMVECTOR p = XMLoadFloat3(&live->position);
        const XMVECTOR offset = XMVectorSet(
            shake.posAmplitude.x * strength * nx,
            shake.posAmplitude.y * strength * ny,
            shake.posAmplitude.z * strength * nz,
            0.f);
        XMStoreFloat3(&live->position, XMVectorAdd(p, offset));

        // Additive rotation: build a small-angle quaternion from axis*amp*str.
        const XMVECTOR axis = XMVector3Normalize(
            XMVectorSet(nx, ny, nz, 0.f));
        const float angle = strength *
            (std::abs(shake.rotAmplitude.x) +
             std::abs(shake.rotAmplitude.y) +
             std::abs(shake.rotAmplitude.z)) / 3.f;
        const XMVECTOR dq = XMQuaternionRotationAxis(axis, angle);
        XMVECTOR q = XMQuaternionNormalize(XMLoadFloat4(&live->rotation));
        q = XMQuaternionMultiply(q, dq);
        XMStoreFloat4(&live->rotation, q);

        // Drain trauma.
        shake.trauma = std::max(0.f, shake.trauma - shake.falloffPerSec * dt);
    });
}

// ==== CameraMotionTickSystem: plays a camera-animation clip onto a VCam ====
namespace
{
    // Sample the dense per-frame CameraAnimKey stream at time t (seconds).
    // Lerp position, shortest-path nlerp rotation, lerp FOV. Does NOT
    // interpolate across a cut: holds the pre-cut frame until the cut frame
    // is reached (the cut frame itself carries the post-cut pose).
    Resource::CameraAnimKey SampleCameraClip(
        const std::vector<Resource::CameraAnimKey>& keys, float frameRate, float t)
    {
        const uint32_t n = static_cast<uint32_t>(keys.size());
        if (n == 0) return {};
        if (n == 1) return keys[0];

        const float fr = (frameRate > 0.f) ? frameRate : 30.f;
        const float frame = t * fr;
        if (frame <= 0.f)               return keys[0];
        if (frame >= float(n - 1))      return keys[n - 1];

        const uint32_t f0 = static_cast<uint32_t>(frame);
        const uint32_t f1 = f0 + 1;
        const float    a  = frame - static_cast<float>(f0);
        const Resource::CameraAnimKey& k0 = keys[f0];
        const Resource::CameraAnimKey& k1 = keys[f1];

        if (k1.flags & Resource::CAMERA_KEY_CUT) return k0; // step at the cut

        Resource::CameraAnimKey out = k0;  // inherits near/far/ortho/flags
        XMStoreFloat3(&out.position,
            XMVectorLerp(XMLoadFloat3(&k0.position), XMLoadFloat3(&k1.position), a));
        XMVECTOR q0 = XMLoadFloat4(&k0.rotation);
        XMVECTOR q1 = XMLoadFloat4(&k1.rotation);
        if (XMVectorGetX(XMVector4Dot(q0, q1)) < 0.f) q1 = XMVectorNegate(q1);
        XMStoreFloat4(&out.rotation, XMQuaternionNormalize(XMVectorLerp(q0, q1, a)));
        out.fovY = k0.fovY + a * (k1.fovY - k0.fovY);
        return out;
    }

    // True if any integer frame in (prevT, currT] is flagged as a hard cut, or
    // on the very first sample / a backward (looped/scrubbed) jump.
    bool CrossedCut(const std::vector<Resource::CameraAnimKey>& keys,
                    float frameRate, float prevT, float currT)
    {
        if (prevT < 0.f)    return true;   // first sample -> clear temporal history
        if (currT < prevT)  return true;   // looped or scrubbed backward
        const float fr = (frameRate > 0.f) ? frameRate : 30.f;
        const int p = static_cast<int>(std::floor(prevT * fr));
        const int c = static_cast<int>(std::floor(currT * fr));
        const int n = static_cast<int>(keys.size());
        for (int f = p + 1; f <= c && f < n; ++f)
            if (keys[f].flags & Resource::CAMERA_KEY_CUT) return true;
        return false;
    }
}

void CameraMotionTickSystem::Update(World& world, const FrameContext& ctx)
{
    if (!m_clips) return;

    world.ForEach<CameraMotionComponent>([&](Entity e, CameraMotionComponent& cm)
    {
        // Stop request (inspector / Lua): release the VCam and halt.
        if (cm.stopRequested)
        {
            cm.stopRequested = false;
            if (cm.pushed) { Camera::PopVCam(world, e, 0.2f); cm.pushed = false; }
            cm.playing = false;
            cm.paused  = false;
        }

        // Auto-start once when the sim begins advancing (PIE / Game build).
        if (cm.playOnLoad && !cm.started && !cm.playing && ctx.runUpdate &&
            !cm.motionPath.empty())
        {
            cm.playing  = true;
            cm.started  = true;
            cm.time     = 0.f;
            cm.prevTime = -1.f;
        }
        if (!cm.playing) return;

        // Acquire the camera .ianim (async; dedup by path).
        if (!cm.handleRequested && !cm.motionPath.empty())
        {
            cm.handle          = m_clips->AcquireClip(cm.motionPath);
            cm.handleRequested = true;
        }
        if (!m_clips->IsReady(cm.handle)) return;

        const Resource::AnimationResource* res = m_clips->GetResource(cm.handle);
        if (!res || res->cameraClips.empty()) return;
        const Resource::CameraAnimClipData& clip = res->cameraClips[0];
        if (clip.channels.empty() || clip.channels[0].keys.empty()) return;
        const std::vector<Resource::CameraAnimKey>& keys = clip.channels[0].keys;

        const CameraChannelId channel =
            cm.channelId ? cm.channelId : Camera::kMainChannel;

        // Take over the channel exactly once: push our VCam + hard cut so the
        // temporal-history passes clear at the switch.
        if (!cm.pushed)
        {
            Camera::PushVCamArgs args{};
            args.priority         = cm.priority;
            args.blendInDuration  = 0.f;     // cinematic hard take-over
            args.blendOutDuration = 0.25f;
            args.channelId        = channel;
            Camera::PushVCam(world, e, args);
            cm.pushed = true;

            if (Entity chE = Camera::GetOrCreateChannelEntity(world, channel);
                chE != NullEntity)
                if (auto* sing = world.GetComponent<CameraStackChannelSingleton>(chE))
                    sing->requestHardCut = true;
        }

        const float dur = (clip.duration > 0.f)
            ? clip.duration
            : (clip.frameRate > 0.f ? clip.frameCount / clip.frameRate : 0.f);

        // Advance playback time. Frozen while the sim is not advancing (editor Stopped /
        // Paused -> dt = 0), like the skinned AnimationSystem; one Step advances one 1/60 s
        // step. With externalTime a MotionDirector or Sequencer already set cm.time and owns
        // loop / clamp: only sample.
        bool finished = false;
        if (!cm.externalTime)
        {
            const float dt = ctx.runUpdate ? ctx.deltaTime : 0.0f;
            if (!cm.paused)
                cm.time += dt * cm.speed;

            if (cm.looping)
            {
                if (dur > 1e-6f) cm.time = std::fmod(cm.time, dur);
            }
            else if (cm.time >= dur)
            {
                cm.time  = dur;
                finished = true;
            }
        }

        // Sample + cut detection.
        const Resource::CameraAnimKey k = SampleCameraClip(keys, clip.frameRate, cm.time);
        if (CrossedCut(keys, clip.frameRate, cm.prevTime, cm.time))
            if (Entity chE = Camera::FindChannelEntity(world, channel); chE != NullEntity)
                if (auto* sing = world.GetComponent<CameraStackChannelSingleton>(chE))
                    sing->requestHardCut = true;
        cm.prevTime = cm.time;

        // Drive the VCam pose + lens. The clip's poses are in MMD world space (subject
        // framed at the clip origin); compose them onto this entity's GlobalTransform, the
        // "stage root", so the whole shot can be placed, rotated and scaled. An identity
        // transform reproduces the raw clip. translationScale converts the clip's position
        // units before the base transform and does not touch the stage-root transform or
        // the lens. Without a GlobalTransform the scaled clip pose is used.
        if (auto* pose = world.GetComponent<CameraPoseComponent>(e))
        {
            const XMVECTOR clipPos = XMVectorScale(
                XMLoadFloat3(&k.position), cm.translationScale);
            bool composed = false;
            if (const auto* gt = world.GetComponent<GlobalTransform>(e))
            {
                const XMMATRIX base = XMLoadFloat4x4(&gt->matrix);
                XMVECTOR bScale, baseRot, bTrans;
                if (XMMatrixDecompose(&bScale, &baseRot, &bTrans, base))
                {
                    const XMVECTOR clipRot = XMLoadFloat4(&k.rotation);
                    // Position: clip-local camera point placed in the base frame
                    // (honors base translation / rotation / scale).
                    XMStoreFloat3(&pose->position,
                        XMVector3TransformCoord(clipPos, base));
                    // Rotation: clip orientation first, then the base orientation; keeps +Z forward /
                    // +Y up.
                    XMStoreFloat4(&pose->rotation,
                        XMQuaternionNormalize(XMQuaternionMultiply(clipRot, baseRot)));
                    composed = true;
                }
            }
            if (!composed)               // no / degenerate transform -> raw clip pose
            {
                XMStoreFloat3(&pose->position, clipPos);
                pose->rotation = k.rotation;
            }
        }
        if (auto* vc = world.GetComponent<VirtualCameraComponent>(e))
        {
            vc->fov   = k.fovY;
            vc->nearZ = k.nearZ;
            vc->farZ  = k.farZ;
        }

        // Release the channel when a non-looping motion finishes.
        if (finished)
        {
            if (cm.pushed) { Camera::PopVCam(world, e, 0.3f); cm.pushed = false; }
            cm.playing = false;
        }
    });
}

// ==== MotionDirectorTickSystem: one master clock drives N synchronized tracks ====
namespace
{
    // Find a skinned track's AnimationComponent: the authored target is often the
    // prefab / scene ROOT while the component lives on a descendant (skinned .iscn
    // subtree). Target first, then the subtree by iterative DFS (deep hierarchies
    // overflow recursion); the carrier is cached on the track.
    AnimationComponent* ResolveSkinnedAnim(World& world,
                                           const MotionDirectorTrack& tr,
                                           Entity target)
    {
        if (tr.animEntity != NullEntity && world.IsAlive(tr.animEntity))
            if (auto* anim = world.GetComponent<AnimationComponent>(tr.animEntity))
                return anim;
        tr.animEntity = NullEntity;

        std::vector<Entity> stack{ target };
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!world.IsAlive(e)) continue;
            if (auto* anim = world.GetComponent<AnimationComponent>(e))
            {
                tr.animEntity = e;
                return anim;
            }
            if (const Children* ch = world.GetComponent<Children>(e))
                for (Entity child : ch->entities) stack.push_back(child);
        }
        return nullptr;
    }

    // Hand each track back to its own component: stop camera and audio tracks, give a
    // skinned track its clock back but leave it paused where the performance ended.
    void ReleaseDirectorTracks(World& world, MotionDirectorComponent& d)
    {
        for (MotionDirectorTrack& tr : d.tracks)
        {
            tr.fired = false;                       // re-arm audio cues for the next play
            const Entity target = tr.target.Resolve(world);
            if (target == NullEntity) continue;
            if (tr.type == MotionDirectorTrack::Camera)
            {
                if (auto* cm = world.GetComponent<CameraMotionComponent>(target))
                {
                    cm->externalTime  = false;
                    cm->stopRequested = true;   // CameraMotionTickSystem pops the VCam
                }
            }
            else if (tr.type == MotionDirectorTrack::Audio)
            {
                if (auto* a = world.GetComponent<Audio::AudioSourceComponent>(target))
                    a->stopRequested = true;    // AudioSystem stops + releases the voice
            }
            else
            {
                if (auto* anim = ResolveSkinnedAnim(world, tr, target))
                {
                    anim->externalTime = false; // hand the clock back...
                    anim->paused       = true;  // ...but freeze where the performance ended
                }
            }
        }
    }
}

void MotionDirectorTickSystem::Update(World& world, const FrameContext& ctx)
{
    world.ForEach<MotionDirectorComponent>([&](Entity /*e*/, MotionDirectorComponent& d)
    {
        if (d.stopRequested)
        {
            d.stopRequested = false;
            ReleaseDirectorTracks(world, d);
            d.playing = false;
            d.paused  = false;
            d.tracksDiagnosed = false;
        }

        // Auto-start once when the sim begins advancing (PIE / Game build).
        if (d.playOnLoad && !d.started && !d.playing && ctx.runUpdate)
        {
            d.playing    = true;
            d.started    = true;
            d.masterTime = 0.f;
        }
        if (!d.playing) { d.tracksDiagnosed = false; return; }

        // Once per play, after 1 s so deferred clip binds have settled: log a track that
        // does not resolve (usually a stale target GUID after a scene edit), the "one
        // layer isn't moving" data bug.
        if (!d.tracksDiagnosed && d.masterTime > 1.f)
        {
            d.tracksDiagnosed = true;
            for (size_t i = 0; i < d.tracks.size(); ++i)
            {
                const MotionDirectorTrack& tr = d.tracks[i];
                static const char* kTypeNames[] = { "Skinned", "Camera", "Audio" };
                const char* typeName = tr.type <= 2 ? kTypeNames[tr.type] : "?";
                const Entity target = tr.target.Resolve(world);
                if (target == NullEntity)
                {
                    LOG_WARNING("MotionDirector: track %zu (%s) target GUID does not "
                                "resolve to any entity -- track is NOT playing",
                                i, typeName);
                    continue;
                }
                bool ok = true;
                if (tr.type == MotionDirectorTrack::Skinned)
                    ok = ResolveSkinnedAnim(world, tr, target) != nullptr;
                else if (tr.type == MotionDirectorTrack::Camera)
                    ok = world.GetComponent<CameraMotionComponent>(target) != nullptr;
                else
                    ok = world.GetComponent<Audio::AudioSourceComponent>(target) != nullptr;
                if (!ok)
                    LOG_WARNING("MotionDirector: track %zu (%s) target entity %u has no "
                                "matching component (searched subtree for Skinned) -- "
                                "track is NOT playing", i, typeName,
                                static_cast<uint32_t>(target));
            }
        }

        // Advance the master clock. Frozen while the sim is not advancing (editor
        // Stopped / Paused -> dt = 0), so camera and skinned tracks freeze in lockstep; one
        // Step advances one 1/60 s step.
        const float dt = ctx.runUpdate ? ctx.deltaTime : 0.0f;
        if (!d.paused && !d.externalTime)
            d.masterTime += dt * d.masterSpeed;
        bool wrapped = false;
        // externalTime: a Sequencer Director track owns masterTime and already placed it
        // this frame; advancing, wrapping or clamping it here would fight that absolute
        // write. This tick is then a pure fan-out of that clock.
        if (d.duration > 1e-6f && !d.externalTime)
        {
            if (d.looping)
            {
                if (d.masterTime >= d.duration) wrapped = true;  // loop boundary crossed
                d.masterTime = std::fmod(d.masterTime, d.duration);
            }
            else if (d.masterTime > d.duration) d.masterTime = d.duration;
        }

        // ---- Auto-resolve the total span for countdown / UI (duration == 0) ----
        // With no authored length, take max(timeOffset + clipDur / speedMul) over the
        // non-audio tracks once their clips have streamed in. Retry each frame
        // (resolvedDuration stays 0) until EVERY non-audio track is ready, so the published
        // end is the real one. Audio cannot scrub, so it does not define the span. Without
        // the clip sources (m_cameraClips / m_skinnedClips) resolvedDuration stays 0.
        if (d.duration <= 1e-6f && d.resolvedDuration <= 0.f)
        {
            float span    = 0.f;
            bool  any     = false;   // found at least one measurable track
            bool  pending = false;   // a track's clip is still loading
            for (const MotionDirectorTrack& tr : d.tracks)
            {
                if (tr.type == MotionDirectorTrack::Audio) continue;
                const Entity target = tr.target.Resolve(world);
                if (target == NullEntity) continue;
                const float spd = (tr.speedMul > 1e-6f) ? tr.speedMul : 1.f;
                float clipDur = 0.f;

                if (tr.type == MotionDirectorTrack::Skinned)
                {
                    const auto* anim = ResolveSkinnedAnim(world, tr, target);
                    if (!anim || anim->primaryClip == kInvalidAnimHandle ||
                        !m_skinnedClips || anim->primaryClip >= m_skinnedClips->Count())
                    { pending = true; continue; }
                    clipDur = m_skinnedClips->Get(anim->primaryClip).duration;
                }
                else // Camera
                {
                    const auto* cm = world.GetComponent<CameraMotionComponent>(target);
                    if (!cm || cm->motionPath.empty()) continue;   // nothing to measure
                    if (!m_cameraClips || !cm->handleRequested ||
                        !m_cameraClips->IsReady(cm->handle))
                    { pending = true; continue; }
                    const Resource::AnimationResource* res = m_cameraClips->GetResource(cm->handle);
                    if (!res || res->cameraClips.empty()) { pending = true; continue; }
                    const Resource::CameraAnimClipData& cc = res->cameraClips[0];
                    clipDur = (cc.duration > 0.f)
                            ? cc.duration
                            : (cc.frameRate > 0.f ? cc.frameCount / cc.frameRate : 0.f);
                }

                if (clipDur > 0.f)
                {
                    span = std::max(span, tr.timeOffset + clipDur / spd);
                    any  = true;
                }
            }
            if (any && !pending) d.resolvedDuration = span;
        }

        // Distribute the master time to every track. Each target keeps its own
        // clip/asset; we only set its playback time + externalTime gate so the
        // AnimationSystem / CameraMotionTickSystem sample in lockstep.
        for (MotionDirectorTrack& tr : d.tracks)
        {
            const Entity target = tr.target.Resolve(world);
            if (target == NullEntity) continue;

            // Audio tracks are one-shot cues (audio cannot scrub): fire the source's Play once
            // the master clock crosses timeOffset, follow the director's pause, and re-arm on
            // loop so the cue repeats each cycle.
            if (tr.type == MotionDirectorTrack::Audio)
            {
                auto* a = world.GetComponent<Audio::AudioSourceComponent>(target);
                if (!a) continue;
                if (wrapped) tr.fired = false;                  // new cycle -> re-arm
                const float rawLocal = d.masterTime - tr.timeOffset;
                if (rawLocal < 0.f)                tr.fired = false;   // before the cue
                else if (!tr.fired) { a->playRequested = true; tr.fired = true; }
                a->paused = d.paused;                           // director pause -> audio pause
                continue;
            }

            float local = (d.masterTime - tr.timeOffset) * tr.speedMul;
            if (local < 0.f) local = 0.f;

            if (tr.type == MotionDirectorTrack::Camera)
            {
                if (auto* cm = world.GetComponent<CameraMotionComponent>(target))
                {
                    cm->externalTime = true;
                    cm->paused       = false;
                    cm->playing      = true;   // ensure it pushes its VCam + samples
                    cm->time         = local;
                }
            }
            else // Skinned
            {
                if (auto* anim = ResolveSkinnedAnim(world, tr, target))
                {
                    anim->externalTime = true;
                    anim->paused       = false;
                    anim->primaryTime  = local;
                }
            }
        }
    });
}

// ==== DeclareAccess: scheduler co-batching declarations ====
// The camera pipeline has real data dependencies (Stack -> Resolve -> Shake), so
// these mostly document correctness and let UNRELATED systems (audio, mailbox
// drainers) run alongside. AttachmentRef::Resolve's GuidRegistry Find() is a
// read-only lookup (ExclusiveResourceIds.h, kGuidRegistry). CameraMotionTickSystem
// stays opaque: see its header comment.

void FollowCameraTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Write<FollowCameraComponent>, not Read: target.Resolve() writes the
    // mutable AttachmentRef cache (cachedOwner/cachedGeneration) embedded in
    // the component.
    b.Read<FollowCameraTag>()
     .Write<FollowCameraComponent>()
     .Write<CameraPoseComponent>()
     .Read<GlobalTransform>();
}

void AimCameraTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Write<AimCameraComponent>: pitch clamp + the target AttachmentRef's mutable
    // resolve cache. The optional wall probe runs a Jolt narrow-phase CastShape
    // (read-only query + per-body shared lock) -> kPhysicsWorld by engine convention.
    b.Read<AimCameraTag>()
     .Write<AimCameraComponent>()
     .Write<CameraPoseComponent>()
     .Read<GlobalTransform>()
     .Read<CharacterControllerComponent>()  // target's own body, excluded from the probe
     .Read<RigidBodyComponent>()
     .ExclusiveResource(ExclusiveRes::kPhysicsWorld);
}

void MotionDirectorTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Master clock: writes per-track fired flags + AttachmentRef caches on
    // its own component, then drives camera tracks (CameraMotionComponent),
    // skinned tracks (AnimationComponent.primaryTime/externalTime) and audio
    // cue tracks (AudioSourceComponent play/stop/pause flags).
    b.Write<MotionDirectorComponent>()
     .Write<CameraMotionComponent>()
     .Write<AnimationComponent>()
     .Write<Audio::AudioSourceComponent>();
}

void CameraStackTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Bridge pass mirrors GlobalTransform into CameraPoseComponent for
    // behavior-less VCams; stack pass rebuilds channel entries and advances
    // every VCam's blend state machine.
    b.Read<VirtualCameraComponent>()
     .Read<VCamPriorityComponent>()
     .Read<FollowCameraTag>()
     .Read<AimCameraTag>()
     .Read<CameraMotionComponent>()
     // The stack pass reads the Sequencer's ownership tag to publish, not advance,
     // sequenced VCam blends; undeclared, a batch-mate writing the tag pool would race
     // this read.
     .Read<SeqCameraOwnedTag>()
     .Read<GlobalTransform>()
     .Write<CameraPoseComponent>()
     .Write<VCamBlendComponent>()
     .Write<CameraStackChannelSingleton>();
}

void CameraResolveTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Blends contributing VCam poses into LiveCameraComponent; consumes +
    // clears the channel's requestHardCut flag (hence Write on the
    // singleton).
    b.Read<VCamBlendComponent>()
     .Read<CameraPoseComponent>()
     .Read<VirtualCameraComponent>()
     .Write<CameraStackChannelSingleton>()
     .Write<LiveCameraComponent>();
}

void CameraShakeTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Drains trauma on the shake component and perturbs the resolved Live
    // pose additively.
    b.Write<CameraShakeComponent>()
     .Write<LiveCameraComponent>();
}
