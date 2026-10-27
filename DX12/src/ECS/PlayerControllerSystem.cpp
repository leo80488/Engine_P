#include "ECS/PlayerControllerSystem.h"
#include "ECS/PlayerComponent.h"
#include "ECS/CharacterIntentComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/ECS.h"
#include "Input/InputSystem.h"
#include "System/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // VK_SPACE / VK_LSHIFT
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

// PlayerControllerSystem: the player brain. It reads Input and the camera basis and writes
// CharacterIntentComponent only; it never touches CharacterControllerComponent or rotation.
// CharacterMotorSystem (end of the AI phase) turns intent into CCC drive fields, jump buffering /
// coyote time and facing, the same way for every brain.

// Toggle: 0 silent, 1 throttled snapshot + edge events, 2 every frame.
#ifndef PLAYER_DEBUG
#define PLAYER_DEBUG 1
#endif

namespace
{
    struct PlayerPrev {
        bool basisOK = true;
    };
    std::unordered_map<Entity, PlayerPrev> s_playerPrev;

    inline bool ShouldThrottleLog()
    {
        static int s_counter = 0;
        ++s_counter;
        return (s_counter % 30) == 0;   // ~ twice per second at 60 Hz
    }
}

using namespace DirectX;

namespace
{
    // Camera forward / right projected onto the XZ plane and re-normalised: the WASD basis
    // (forward = +W, right = +D). Returns false when the camera entity has no GlobalTransform or its
    // forward is degenerate (looking straight up / down).
    bool ResolveCameraBasis(World& world, Entity cam,
                            XMVECTOR& outForward, XMVECTOR& outRight)
    {
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(cam);
        if (!gt) return false;

        const XMMATRIX worldMatrix = XMLoadFloat4x4(&gt->matrix);
        XMVECTOR fwd = worldMatrix.r[2];

        fwd = XMVectorSetY(fwd, 0.f);
        const float lenSq = XMVectorGetX(XMVector3LengthSq(fwd));
        if (lenSq < 1e-6f) return false;
        fwd = XMVector3Normalize(fwd);

        const XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        XMVECTOR right = XMVector3Cross(up, fwd);     // LH: right = up x forward
        right = XMVector3Normalize(right);

        outForward = fwd;
        outRight   = right;
        return true;
    }
}

void PlayerControllerSystem::Update(World& world, Entity mainCamera, float dt)
{
    (void)dt;   // continuous intent - the Motor owns all time integration

    auto* playerPool = world.GetPool<PlayerComponent>();
    if (!playerPool) return;
    world.EnsurePool<CharacterIntentComponent>();

    // Device-agnostic: the action layer hides whether a keyboard or a gamepad produced these.
    // Sampled once, outside the entity loop, from the immutable per-frame snapshot latched in
    // Input::Update.
    const ActionState&   cmd = Input::Get().Cmd();
    const InputBindings& cfg = Input::Get().Bindings();
    const bool  sprint   = cmd.Held(GameAction::Sprint);
    const float ix       = cmd.moveX;
    const float iz       = cmd.moveY;
    const bool  jumpEdge = cmd.Pressed(GameAction::Jump);

    const auto& entities = playerPool->Entities();
    auto&       players  = playerPool->Data();
    const size_t n = players.size();

#if PLAYER_DEBUG >= 1
    const bool throttleFire = ShouldThrottleLog();
    if (throttleFire)
    {
        LOG_INFO("[Player] frame snapshot: nPlayers=%zu input(ix=%.1f iz=%.1f sprint=%d jumpEdge=%d) mainCamHint=%u",
                 n, ix, iz, sprint ? 1 : 0, jumpEdge ? 1 : 0,
                 static_cast<uint32_t>(mainCamera));
    }
#endif

    for (size_t i = 0; i < n; ++i)
    {
        PlayerComponent& pc = players[i];
        const Entity e = entities[i];

        // Possession gate - another brain owns this entity.
        if (!BrainAllows(world.GetComponent<BrainComponent>(e), BrainType::Player))
            continue;

        // Camera basis (explicit ref via GUID, else App-supplied main camera,
        // else world axes so input never freezes).
        const Entity resolvedCam = pc.cameraEntity.Resolve(world);
        const Entity camE = (resolvedCam != NullEntity) ? resolvedCam : mainCamera;
        XMVECTOR fwd, right;
        const bool basisOK = ResolveCameraBasis(world, camE, fwd, right);
        if (!basisOK)
        {
            fwd   = XMVectorSet(0.f, 0.f, 1.f, 0.f);
            right = XMVectorSet(1.f, 0.f, 0.f, 0.f);
        }
#if PLAYER_DEBUG >= 1
        auto& prev = s_playerPrev[e];
        if (prev.basisOK != basisOK)
        {
            LOG_INFO("[Player ent=%u] camera basis %s (camEntity=%u)",
                     e, basisOK ? "RESOLVED" : "FALLBACK (camera missing or looking ±Y)",
                     static_cast<uint32_t>(camE));
            prev.basisOK = basisOK;
        }
#endif

        // Compose + normalise the horizontal direction (diagonal must not be
        // faster than cardinal).
        XMVECTOR dir = XMVectorAdd(XMVectorScale(right, ix), XMVectorScale(fwd, iz));
        const float dirLenSq = XMVectorGetX(XMVector3LengthSq(dir));
        if (dirLenSq > 1e-6f)
            dir = XMVector3Normalize(dir);
        else
            dir = XMVectorZero();

        // ---- Write intent ----
        CharacterIntentComponent* it =
            world.GetComponent<CharacterIntentComponent>(e);
        if (!it)
        {
            world.AddComponent<CharacterIntentComponent>(e, {});
            it = world.GetComponent<CharacterIntentComponent>(e);
            if (!it) continue;
        }

        XMFLOAT3 dirF; XMStoreFloat3(&dirF, dir);

        // moveDir stays a UNIT vector. CharacterMotorSystem computes
        // target = moveDir.xz * desiredSpeed and gates facing on
        // |moveDir.xz|^2 > 1e-6, so analog magnitude must live in desiredSpeed
        // ONLY. Encoding it in both scales speed quadratically.
        it->moveDir = { dirF.x, 0.f, dirF.z };

        // ---- Speed: the one place magnitude could be applied, and is not ----
        float speed = 0.f;
        if (dirLenSq > 1e-6f && cmd.moveMag > 1e-4f)
        {
            // Stick deflection does not affect speed: any input past the deadzone is full speed for the
            // current gait, like a keyboard key. The stick contributes direction only (a full 360 deg moveDir).
            // Rejected: magnitude selecting the gait (cannot walk at full tilt; a worn stick never runs) and
            // magnitude scaling speed (a light touch creeps below LocomotionTransitionComponent's 0.25 m/s
            // Start threshold, so the character slides while the blend space plays idle). Full speed on any
            // deflection is also why no speed floor is needed.
            speed = sprint ? pc.runSpeed : pc.walkSpeed;
        }
        it->desiredSpeed = speed;

        // Gait is an annotation for animation (CharacterAnimDriver passes it to the anim graph's gait
        // parameter), not a behaviour switch: the blend space is driven by real post-KCC velocity. Do not
        // build gameplay on it. It mirrors the input, not the resulting speed: Sprint held = Run, else Walk.
        it->gait = sprint ? CharacterGait::Run : CharacterGait::Walk;

        // Held state; the Motor copies it to the KCC's wantsCrouch. Crouch ships unbound by default
        // because nothing reads wantsCrouch yet.
        it->crouch = cmd.Held(GameAction::Crouch);

        // ---- Facing intent ----
        // FaceMovement (the Motor default) leaves useFaceDir clear and the Motor turns the body into
        // moveDir. The other modes hand the Motor an explicit target, so moveDir can point sideways or
        // backwards relative to the body - which is what lets a Blend2D LocomotionComponent see angles
        // other than 0 (CharacterAnimDriver derives dirDeg from velocity vs the body's forward axis).
        switch (pc.facingMode)
        {
            case PlayerFacingMode::FaceCamera:
            {
                XMFLOAT3 camF; XMStoreFloat3(&camF, fwd);
                it->faceDir    = { camF.x, 0.f, camF.z };
                it->useFaceDir = true;
                break;
            }
            case PlayerFacingMode::Locked:
            {
                // Target = the body's CURRENT forward, so the Motor's turn
                // toward it is a no-op. Keeps one facing code path instead of
                // teaching the Motor a "don't rotate" special case.
                const auto* lt = world.GetComponent<LocalTransform>(e);
                const XMVECTOR q = lt ? XMLoadFloat4(&lt->rotation)
                                      : XMQuaternionIdentity();
                XMFLOAT3 bodyF;
                XMStoreFloat3(&bodyF, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q));
                it->faceDir    = { bodyF.x, 0.f, bodyF.z };
                it->useFaceDir = true;
                break;
            }
            case PlayerFacingMode::FaceMovement:
            default:
                it->useFaceDir = false;        // face movement (Motor default)
                break;
        }

        if (jumpEdge) it->jump = true;         // edge - the Motor buffers it and applies coyote time

#if PLAYER_DEBUG >= 1
        if (throttleFire || PLAYER_DEBUG >= 2)
        {
            LOG_INFO("[Player ent=%u] intent dir=(%.2f,%.2f) speed=%.2f gait=%d jump=%d",
                     e, it->moveDir.x, it->moveDir.z, it->desiredSpeed,
                     static_cast<int>(it->gait), it->jump ? 1 : 0);
        }
#endif
    }
}
