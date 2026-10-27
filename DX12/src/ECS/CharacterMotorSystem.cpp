#include "ECS/CharacterMotorSystem.h"

#include "ECS/ECS.h"
#include "ECS/CharacterIntentComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/PlayerComponent.h"
#include "ECS/AnimGraphComponent.h"
#include "ECS/AnimEntityResolve.h"
#include "ECS/HierarchyComponents.h"     // LocalTransform
#include "Nav/NavComponents.h"           // NavAgentComponent (turn tuning)

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
    constexpr float kPi  = 3.14159265358979323846f;
    constexpr float kTau = 2.f * kPi;

    // Player-style facing: constant-rate shortest-arc yaw clamp (turnRate in rad/s).
    void FaceClampRate(LocalTransform& lt, float dirX, float dirZ,
                       float turnRate, float dt)
    {
        const float targetYaw = std::atan2(dirX, dirZ);
        const XMVECTOR currQ   = XMLoadFloat4(&lt.rotation);
        const XMVECTOR currFwd = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), currQ);
        XMFLOAT3 cf; XMStoreFloat3(&cf, currFwd);
        const float currYaw = std::atan2(cf.x, cf.z);

        float dyaw = targetYaw - currYaw;
        while (dyaw >  kPi) dyaw -= kTau;
        while (dyaw < -kPi) dyaw += kTau;

        const float maxStep = turnRate * dt;
        if (dyaw >  maxStep) dyaw =  maxStep;
        if (dyaw < -maxStep) dyaw = -maxStep;

        const XMVECTOR newQ = XMQuaternionRotationAxis(
            XMVectorSet(0.f, 1.f, 0.f, 0.f), currYaw + dyaw);
        XMStoreFloat4(&lt.rotation, newQ);
    }

    // NavAgent-style facing: NLerp with t = turnRate * dt (exponential ease). Kept
    // separate because NavAgent turnRate values are tuned as lerp gains, not rad/s.
    void FaceNLerp(LocalTransform& lt, float dirX, float dirZ,
                   float turnRate, float dt)
    {
        const float lenXZ = std::sqrt(dirX * dirX + dirZ * dirZ);
        if (lenXZ < 1e-5f) return;
        const float yaw = std::atan2(dirX, dirZ);
        const XMVECTOR want = XMQuaternionRotationRollPitchYaw(0.f, yaw, 0.f);

        XMVECTOR va = XMLoadFloat4(&lt.rotation);
        XMVECTOR vb = want;
        if (XMVectorGetX(XMVector4Dot(va, vb)) < 0.0f) vb = XMVectorNegate(vb);
        const float t = std::min(1.f, turnRate * dt);
        XMVECTOR mixed = XMVectorAdd(XMVectorScale(va, 1.f - t),
                                     XMVectorScale(vb, t));
        mixed = XMQuaternionNormalize(mixed);
        XMStoreFloat4(&lt.rotation, mixed);
    }
}

namespace Character
{

void MotorTick(::World& world, float dt)
{
    if (dt <= 0.f) return;

    auto* pool = world.GetPool<CharacterIntentComponent>();
    if (!pool) return;

    const auto&  entities = pool->Entities();
    auto&        intents  = pool->Data();
    const size_t n        = intents.size();

    for (size_t i = 0; i < n; ++i)
    {
        const Entity e = entities[i];
        CharacterIntentComponent& it = intents[i];

        CharacterControllerComponent* cc =
            world.GetComponent<CharacterControllerComponent>(e);
        if (!cc) continue;   // intent without a motor: nothing to drive

        const PlayerComponent*   pc  = world.GetComponent<PlayerComponent>(e);
        const NavAgentComponent* nav = world.GetComponent<NavAgentComponent>(e);

        // ---- Motor override (dash / knockback / launch) ----
        // Replaces the brain's intent outright, including the accel ramp and airControl:
        // a dash that decays through groundAccel is not a dash. The countdown lives here
        // because this is the only place with a reliable per-entity dt; the caller only
        // says "for N seconds".
        const bool moveLocked = it.lockTimer > 0.f;
        if (moveLocked)
        {
            it.lockTimer = std::max(0.f, it.lockTimer - dt);
            cc->desiredHorizontalVelocity = { it.lockDir.x * it.lockSpeed, 0.f,
                                              it.lockDir.z * it.lockSpeed };
        }

        // ---- Horizontal drive ----
        // What the brain is asking for, unscaled.
        const float targetX = it.moveDir.x * it.desiredSpeed;
        const float targetZ = it.moveDir.z * it.desiredSpeed;
        const bool  wantsMove = (it.moveDir.x * it.moveDir.x +
                                 it.moveDir.z * it.moveDir.z) > 1e-6f;

        // Which ramp applies this frame. 0 means "instant" for that case, so a character
        // with all three at 0 gets its velocity assigned outright.
        const float rate = !cc->isGrounded ? cc->airAccel
                         : (wantsMove ? cc->groundAccel : cc->groundDecel);

        if (moveLocked)
        {
            // Already driven above: do not let the brain's intent overwrite it.
        }
        else if (rate <= 0.f)
        {
            // Instant path: assign outright. airControl exists only on PlayerComponent;
            // other characters keep full control in the air (1.0).
            const float airControl   = pc ? pc->airControl : 1.f;
            const float controlScale = cc->isGrounded ? 1.f : airControl;
            cc->desiredHorizontalVelocity = { targetX * controlScale, 0.f,
                                              targetZ * controlScale };
        }
        else
        {
            // Momentum path: step the CURRENT velocity toward the target at a bounded rate.
            // cc->velocity is the velocity the character really had over the last step, so speed
            // carried into a jump persists and airAccel only STEERS it; the ground's own motion
            // (a moving platform) is not the character's to carry.
            float vx = cc->velocity.x - cc->groundVelocity.x;
            float vz = cc->velocity.z - cc->groundVelocity.z;
            const float dx = targetX - vx;
            const float dz = targetZ - vz;
            const float d  = std::sqrt(dx * dx + dz * dz);
            const float step = rate * dt;
            if (d > step && d > 1e-6f) { vx += dx / d * step; vz += dz / d * step; }
            else                       { vx = targetX; vz = targetZ; }
            cc->desiredHorizontalVelocity = { vx, 0.f, vz };
        }

        // Consume the previous completed animation frame exactly once. The
        // motor owns KCC drive and actor facing; the graph only reports deltas.
        auto* graph=world.GetComponent<AnimGraphComponent>(Anim::ResolveAnimEntity(world,e));
        const bool rootDrive=graph && graph->enabled && graph->motorRootMotion && graph->rootFrame!=graph->consumedRootFrame;
        if(graph && graph->rootFrame!=graph->consumedRootFrame)graph->consumedRootFrame=graph->rootFrame;
        if(rootDrive && !moveLocked)if(const auto* transform=world.GetComponent<LocalTransform>(e)){XMFLOAT3 delta;XMStoreFloat3(&delta,XMVector3Rotate(XMLoadFloat3(&graph->rootMotion.translation),XMLoadFloat4(&transform->rotation)));cc->desiredHorizontalVelocity={delta.x/dt,0,delta.z/dt};}
        // ---- Jump (edge -> buffer -> coyote) ----
        // Buffer / coyote times come from PlayerComponent when present, else 0.1 s
        // defaults, so any possessed character jumps with assist. Brains that never set
        // it.jump (NavAgent) are unaffected.
        const float bufferTime = pc ? pc->jumpBufferTime : 0.10f;
        const float coyoteTime = pc ? pc->coyoteTime     : 0.10f;
        cc->jumpGraceTime = coyoteTime;   // the KCC honours a request this long after the ground is left

        if (it.jumpBufferTimer > 0.f)
            it.jumpBufferTimer = std::max(0.f, it.jumpBufferTimer - dt);
        if (it.jump)
        {
            it.jumpBufferTimer = bufferTime;
            it.jump            = false;             // edge consumed
        }
        const bool canJump =
            cc->isGrounded ||
            (cc->timeInAir <= coyoteTime && cc->velocity.y <= 0.f);
        if (it.jumpBufferTimer > 0.f && canJump)
        {
            cc->jumpRequested  = true;
            it.jumpBufferTimer = 0.f;
        }

        // ---- Crouch (stored only: nothing reads wantsCrouch yet) ----
        cc->wantsCrouch = it.crouch;

        // ---- Facing ----
        LocalTransform* lt = world.GetComponent<LocalTransform>(e);
        if (!lt) continue;

        if(rootDrive && !moveLocked){XMStoreFloat4(&lt->rotation,XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&lt->rotation),XMQuaternionRotationRollPitchYaw(0,graph->rootMotion.yaw,0))));continue;}
        float fx = 0.f, fz = 0.f;
        bool  wantFace = false;
        if (moveLocked && it.lockFace)
        {
            // Outranks useFaceDir: a lock-on dodge has to keep pointing where
            // it is travelling for the whole dash, not swing back to the look
            // target halfway through it.
            fx = it.lockDir.x; fz = it.lockDir.z;
            wantFace = (fx * fx + fz * fz) > 1e-6f;
        }
        else if (it.useFaceDir)
        {
            fx = it.faceDir.x; fz = it.faceDir.z;
            const float lenSq = fx * fx + fz * fz;
            wantFace = lenSq > 1e-6f;
            if (wantFace)
            {
                const float inv = 1.f / std::sqrt(lenSq);
                fx *= inv; fz *= inv;
            }
        }
        else
        {
            // Face movement: gate on the APPLIED horizontal speed (facingMinSpeed).
            const float horizSpeed =
                std::sqrt(cc->desiredHorizontalVelocity.x * cc->desiredHorizontalVelocity.x
                        + cc->desiredHorizontalVelocity.z * cc->desiredHorizontalVelocity.z);
            const float minSpeed = pc ? pc->facingMinSpeed : 0.1f;
            if (horizSpeed >= minSpeed)
            {
                fx = it.moveDir.x; fz = it.moveDir.z;
                wantFace = (fx * fx + fz * fz) > 1e-6f;
            }
        }
        if (!wantFace) continue;

        // Style and rate per tuning source, so each keeps its tuned meaning.
        if (pc)
        {
            if (pc->turnRate > 0.f)
                FaceClampRate(*lt, fx, fz, pc->turnRate, dt);
        }
        else if (nav)
        {
            // Look-target facing (useFaceDir) is decided by the brain, which already encodes
            // NavAgent's idle-vs-moving / rotateToFacing rules. Face-movement auto-rotates
            // only in FaceMovement mode with rotateToFacing on (Manual and target-less
            // FaceTarget never do).
            if (it.useFaceDir)
                FaceNLerp(*lt, fx, fz, nav->turnRate, dt);
            else if (nav->rotateToFacing
                  && nav->facingMode == NavFacingMode::FaceMovement)
                FaceNLerp(*lt, fx, fz, nav->turnRate, dt);
        }
        else
        {
            FaceClampRate(*lt, fx, fz, 12.f, dt);
        }
    }
}

} // namespace Character
