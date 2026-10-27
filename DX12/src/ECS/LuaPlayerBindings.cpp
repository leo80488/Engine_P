#include "ECS/LuaPlayerBindings.h"
#include "Scripting/LuaDoc.h"   // bindings described as they are bound
#include "ECS/PlayerComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/CharacterIntentComponent.h"   // move lock (dash / knockback)
#include "ECS/Components.h"           // CameraComponent + ActiveCameraTag
#include "ECS/HierarchyComponents.h"
#include "ECS/ECS.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/CameraStackSystem.h"
#include "System/Log.h"
#include "Scene/TransformSystem.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace
{
    // Generation-aware: a stale id (slot recycled) resolves to NullEntity
    // instead of silently addressing the impostor entity.
    CharacterControllerComponent* GetCC(World& w, LuaEntity v)
    {
        const Entity e = LuaId::ResolveLive(w, v);
        if (auto* cc = e != NullEntity
                     ? w.GetComponent<CharacterControllerComponent>(e) : nullptr)
            return cc;
        LOG_WARNING("Lua: entity %u has no live CharacterControllerComponent",
                    v.raw());
        return nullptr;
    }
    PlayerComponent* GetPC(World& w, LuaEntity v)
    {
        const Entity e = LuaId::ResolveLive(w, v);
        if (auto* p = e != NullEntity
                    ? w.GetComponent<PlayerComponent>(e) : nullptr)
            return p;
        LOG_WARNING("Lua: entity %u has no live PlayerComponent",
                    v.raw());
        return nullptr;
    }
}

void RegisterLuaPlayerBindings(sol::state& lua, World& world)
{
    // ---- Character.* ----
    // Merge into any pre-existing Character table instead of replacing it:
    // RegisterLuaCharacterStateBindings puts AddState / SetState / GetState / IsBlending in the same
    // table, and a fresh create_table() would silently clobber them.
    sol::object existingCh = lua["Character"];
    sol::table ch = existingCh.is<sol::table>()
                  ? existingCh.as<sol::table>()
                  : lua.create_table();

    ch.set_function("IsGrounded",
        [&world](LuaEntity entityId) -> bool {
            auto* cc = GetCC(world, entityId);
            return cc ? cc->isGrounded : false;
        });

    ch.set_function("TimeInAir",
        [&world](LuaEntity entityId) -> float {
            auto* cc = GetCC(world, entityId);
            return cc ? cc->timeInAir : 0.f;
        });

    // Returns world-space velocity (vx, vy, vz). Lua-side: `local x,y,z = Character.GetVelocity(e)`.
    LuaDoc::Fn(ch, "Character", "GetVelocity",
        [&world](LuaEntity entityId) -> std::tuple<float,float,float> {
            auto* cc = GetCC(world, entityId);
            if (!cc) return {0.f, 0.f, 0.f};
            return { cc->velocity.x, cc->velocity.y, cc->velocity.z };
        },
        { .doc = "The velocity the character really had over the last physics step (world space, m/s): its "
                 "displacement over the step, so a character pressed against a wall reads 0 whatever it was told to do, "
                 "and one standing on a moving platform reads the platform's velocity. Vertically, while on the ground "
                 "it is the ground's (a stair step is not a vertical speed). Zeros without a controller.",
          .params = { { .name = "id" } } });

    // World velocity of the ground the character stands on (a moving platform); zero in the air.
    LuaDoc::Fn(ch, "Character", "GetGroundVelocity",
        [&world](LuaEntity entityId) -> std::tuple<float,float,float> {
            auto* cc = GetCC(world, entityId);
            if (!cc) return {0.f, 0.f, 0.f};
            return { cc->groundVelocity.x, cc->groundVelocity.y, cc->groundVelocity.z };
        },
        { .doc = "The world velocity of the ground the character stands on (a kinematic platform moving under it), "
                 "which the character carries along; zero in the air. Subtract it from GetVelocity for the speed "
                 "relative to the ground. Zeros without a controller.",
          .params = { { .name = "id" } } });

    // Ground info: normal + entity ID standing on. Returns (nx, ny, nz, entityId).
    ch.set_function("GetGround",
        [&world](LuaEntity entityId) -> std::tuple<float,float,float,LuaEntity> {
            auto* cc = GetCC(world, entityId);
            if (!cc) return {0.f, 1.f, 0.f, LuaEntity{}};
            return { cc->groundNormal.x, cc->groundNormal.y, cc->groundNormal.z,
                     LuaId::OutMaybe(&world, cc->groundEntity) };
        });

    // Override the drive horizontal velocity for this frame. CharacterMotorSystem rewrites it from intent
    // on every entity it drives (end of the AI phase, after Logic scripts), so this only sticks on a
    // character without intent (an NPC / cinematic puppet); a dash or knockback goes through SetMoveLock.
    ch.set_function("SetDesiredVelocity",
        [&world](LuaEntity entityId, float vx, float vz) -> bool {
            auto* cc = GetCC(world, entityId);
            if (!cc) return false;
            cc->desiredHorizontalVelocity = { vx, 0.f, vz };
            return true;
        });

    // Request a jump on the next physics tick; PhysicsSystem honours it only while grounded. A direct
    // call bypasses CharacterMotorSystem's jump buffer and coyote time.
    ch.set_function("Jump",
        [&world](LuaEntity entityId) -> bool {
            auto* cc = GetCC(world, entityId);
            if (!cc) return false;
            cc->jumpRequested = true;
            return true;
        });

    // Hard teleport: snap LocalTransform.translation, then MarkDirty so PhysicsSystem rebuilds the
    // CharacterVirtual at the new pose (its lazy build reads the entity pose only the first time it
    // sees the controller).
    LuaDoc::Fn(ch, "Character", "Teleport",
        [&world](LuaEntity entityId, float x, float y, float z) -> bool {
            const Entity e = LuaId::ResolveLive(world, entityId);
            auto* cc = GetCC(world, entityId);
            auto* lt = e != NullEntity ? world.GetComponent<LocalTransform>(e) : nullptr;
            if (!cc || !lt || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
            lt->translation = { x, y, z };
            // Physics rebuilds from GlobalTransform before the next normal
            // transform pass. Synchronize ancestors and descendants as well.
            TransformSystem::Propagate(world);
            cc->MarkDirty();   // forces CharacterVirtual rebuild at new pose
            return true;
        },
            { .doc = "Snap a character controller to (x, y, z) and rebuild its physics body there; false without a controller or with a non-finite position.", .params = { { .name = "id" }, { .name = "x" }, { .name = "y" }, { .name = "z" } }, .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Update });

    // Capsule resize; MarkDirty makes PhysicsSystem rebuild the body.
    ch.set_function("SetCapsule",
        [&world](LuaEntity entityId, float radius, float halfHeight) -> bool {
            auto* cc = GetCC(world, entityId);
            if (!cc) return false;
            cc->capsuleRadius     = radius;
            cc->capsuleHalfHeight = halfHeight;
            cc->MarkDirty();
            return true;
        });

    ch.set_function("SetGravity",
        [&world](LuaEntity entityId, float g) -> bool {
            auto* cc = GetCC(world, entityId);
            if (!cc) return false;
            cc->gravity = g;
            return true;
        });

    ch.set_function("SetJumpSpeed",
        [&world](LuaEntity entityId, float s) -> bool {
            auto* cc = GetCC(world, entityId);
            if (!cc) return false;
            cc->jumpSpeed = s;
            return true;
        });

    // ---- Getters ----
    // Every setter has a matching read, so a script that changes a value temporarily can restore the
    // original instead of hard-coding the prefab's value.
    ch.set_function("GetCapsule",
        [&world](LuaEntity entityId) -> std::tuple<float, float> {
            auto* cc = GetCC(world, entityId);
            if (!cc) return { 0.f, 0.f };
            return { cc->capsuleRadius, cc->capsuleHalfHeight };
        });

    ch.set_function("GetGravity",
        [&world](LuaEntity entityId) -> float {
            auto* cc = GetCC(world, entityId);
            return cc ? cc->gravity : 0.f;
        });

    ch.set_function("GetJumpSpeed",
        [&world](LuaEntity entityId) -> float {
            auto* cc = GetCC(world, entityId);
            return cc ? cc->jumpSpeed : 0.f;
        });

    // ---- Momentum ramps ----
    // Character.SetAccel(e, groundAccel, groundDecel, airAccel) -- m/s^2.
    //
    // ALL THREE ZERO (the component default) is not "no acceleration" but the INSTANT-ASSIGN path:
    // desiredHorizontalVelocity is written outright from intent every frame, so there is no inertia and
    // the horizontal speed carried into a jump is discarded on the first airborne frame. A positive
    // value switches that axis to the momentum path, which steps the KCC's post-sweep velocity toward
    // the target.
    //
    // airAccel > 0 REPLACES PlayerComponent::airControl: airControl scales the TARGET, airAccel bounds
    // the RATE, and applying both would fight, so once airAccel is set airControl is inert.
    ch.set_function("SetAccel",
        [&world](LuaEntity entityId, float groundAccel, float groundDecel,
                 float airAccel) -> bool {
            auto* cc = GetCC(world, entityId);
            if (!cc) return false;
            // No MarkDirty: these are read by CharacterMotorSystem every frame,
            // not baked into the CharacterVirtual, so no rebuild is needed.
            cc->groundAccel = std::max(0.f, groundAccel);
            cc->groundDecel = std::max(0.f, groundDecel);
            cc->airAccel    = std::max(0.f, airAccel);
            return true;
        });

    ch.set_function("GetAccel",
        [&world](LuaEntity entityId) -> std::tuple<float, float, float> {
            auto* cc = GetCC(world, entityId);
            if (!cc) return { 0.f, 0.f, 0.f };
            return { cc->groundAccel, cc->groundDecel, cc->airAccel };
        });

    // True when this character is on the momentum path at all -- i.e. whether
    // asking about inertia is even meaningful for it.
    ch.set_function("HasMomentum",
        [&world](LuaEntity entityId) -> bool {
            auto* cc = GetCC(world, entityId);
            return cc && (cc->groundAccel > 0.f || cc->groundDecel > 0.f
                                                || cc->airAccel > 0.f);
        });

    // ---- Motor override (dash / knockback / launch) ----
    // Character.SetDesiredVelocity is USELESS on anything a brain drives: Logic scripts run in
    // GameplayLogic, CharacterMotorSystem runs at the end of the AI phase and rewrites
    // desiredHorizontalVelocity from intent, so the value is silently gone before physics sees it. This
    // is the supported channel: the Motor honours the lock over the brain's horizontal intent and owns
    // the countdown, so a dash survives being driven by the player controller.
    //
    // Character.SetMoveLock(e, dx, dz, speed, duration[, opts{face=}])
    //   dx/dz : world-space XZ direction, normalised here. BOTH ZERO means the direction the character
    //           currently faces (what a forward dash wants).
    //   face  : also pin the body yaw to the lock direction for the duration (default true: a dash
    //           that drifts sideways reads as a slide).
    ch.set_function("SetMoveLock",
        [&world](LuaEntity entityId, float dx, float dz, float speed,
                 float duration, sol::optional<sol::table> optsObj) -> bool {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            if (duration <= 0.f)
            {
                LOG_WARNING("Character.SetMoveLock: duration must be > 0");
                return false;
            }
            world.EnsurePool<CharacterIntentComponent>();
            auto* it = world.GetComponent<CharacterIntentComponent>(e);
            if (!it)
            {
                world.AddComponent<CharacterIntentComponent>(e, {});
                it = world.GetComponent<CharacterIntentComponent>(e);
                if (!it) return false;
            }

            float lenSq = dx * dx + dz * dz;
            if (lenSq <= 1e-8f)
            {
                // Fall back to current facing (+Z is forward in this engine's
                // convention, same basis PlayerControllerSystem uses).
                const auto* lt = world.GetComponent<LocalTransform>(e);
                if (!lt)
                {
                    LOG_WARNING("Character.SetMoveLock: entity %u has no "
                                "LocalTransform and no direction was given",
                                static_cast<uint32_t>(e));
                    return false;
                }
                const DirectX::XMVECTOR q = DirectX::XMLoadFloat4(&lt->rotation);
                const DirectX::XMVECTOR f = DirectX::XMVector3Rotate(
                    DirectX::XMVectorSet(0.f, 0.f, 1.f, 0.f), q);
                DirectX::XMFLOAT3 fwd;
                DirectX::XMStoreFloat3(&fwd, f);
                dx = fwd.x; dz = fwd.z;
                lenSq = dx * dx + dz * dz;
                if (lenSq <= 1e-8f) return false;   // facing straight up/down
            }
            const float inv = 1.f / std::sqrt(lenSq);
            it->lockDir   = { dx * inv, 0.f, dz * inv };
            it->lockSpeed = speed;
            it->lockTimer = duration;
            it->lockFace  = true;
            if (optsObj) it->lockFace = optsObj->get_or("face", true);
            return true;
        });

    ch.set_function("ClearMoveLock",
        [&world](LuaEntity entityId) -> bool {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            auto* it = world.GetComponent<CharacterIntentComponent>(e);
            if (!it) return false;
            it->lockTimer = 0.f;
            it->lockSpeed = 0.f;
            it->lockFace  = false;
            return true;
        });

    ch.set_function("IsMoveLocked",
        [&world](LuaEntity entityId) -> bool {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            const auto* it = world.GetComponent<CharacterIntentComponent>(e);
            return it && it->MoveLocked();
        });

    // Seconds of lock left, 0 when none. Lets a script drive its own state out
    // of the Motor's countdown instead of running a second timer that can
    // drift out of step with it.
    ch.set_function("MoveLockRemaining",
        [&world](LuaEntity entityId) -> float {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return 0.f;
            const auto* it = world.GetComponent<CharacterIntentComponent>(e);
            return it ? std::max(0.f, it->lockTimer) : 0.f;
        });

    lua["Character"] = ch;

    // ---- Player.* ----
    sol::table pl = lua.create_table();

    pl.set_function("SetWalkSpeed",
        [&world](LuaEntity entityId, float s) -> bool {
            auto* pc = GetPC(world, entityId);
            if (!pc) return false;
            pc->walkSpeed = s;
            return true;
        });

    pl.set_function("SetRunSpeed",
        [&world](LuaEntity entityId, float s) -> bool {
            auto* pc = GetPC(world, entityId);
            if (!pc) return false;
            pc->runSpeed = s;
            return true;
        });

    pl.set_function("SetAirControl",
        [&world](LuaEntity entityId, float c) -> bool {
            auto* pc = GetPC(world, entityId);
            if (!pc) return false;
            pc->airControl = c;
            return true;
        });

    // ---- Getters ----
    // A script that temporarily changes a speed (dash, slow, buff) reads the original back instead of
    // hard-coding the prefab's value.
    pl.set_function("GetWalkSpeed",
        [&world](LuaEntity entityId) -> float {
            auto* pc = GetPC(world, entityId);
            return pc ? pc->walkSpeed : 0.f;
        });

    pl.set_function("GetRunSpeed",
        [&world](LuaEntity entityId) -> float {
            auto* pc = GetPC(world, entityId);
            return pc ? pc->runSpeed : 0.f;
        });

    pl.set_function("GetAirControl",
        [&world](LuaEntity entityId) -> float {
            auto* pc = GetPC(world, entityId);
            return pc ? pc->airControl : 0.f;
        });

    pl.set_function("SetTurnRate",
        [&world](LuaEntity entityId, float r) -> bool {
            auto* pc = GetPC(world, entityId);
            if (!pc) return false;
            pc->turnRate = r;
            return true;
        });

    pl.set_function("GetTurnRate",
        [&world](LuaEntity entityId) -> float {
            auto* pc = GetPC(world, entityId);
            return pc ? pc->turnRate : 0.f;
        });

    // Facing mode is not cosmetic on a character with a 2D blend space: the
    // default "FaceMovement" turns the body into the input every frame, so
    // LocomotionComponent::dirDeg stays ~0 and only the forward spoke ever
    // plays. Scripts that set up a directional blend space need to be able to
    // assert the mode rather than rely on the prefab being authored right.
    pl.set_function("SetFacingMode",
        [&world](LuaEntity entityId, const std::string& mode) -> bool {
            auto* pc = GetPC(world, entityId);
            if (!pc) return false;
            if      (mode == "FaceMovement") pc->facingMode = PlayerFacingMode::FaceMovement;
            else if (mode == "FaceCamera")   pc->facingMode = PlayerFacingMode::FaceCamera;
            else if (mode == "Locked")       pc->facingMode = PlayerFacingMode::Locked;
            else
            {
                LOG_WARNING("Player.SetFacingMode: unknown mode '%s' "
                            "(FaceMovement/FaceCamera/Locked)", mode.c_str());
                return false;
            }
            return true;
        });

    pl.set_function("GetFacingMode",
        [&world](LuaEntity entityId) -> std::string {
            auto* pc = GetPC(world, entityId);
            if (!pc) return "";
            switch (pc->facingMode)
            {
                case PlayerFacingMode::FaceCamera: return "FaceCamera";
                case PlayerFacingMode::Locked:     return "Locked";
                default:                           return "FaceMovement";
            }
        });

    pl.set_function("SetCameraEntity",
        [&world](LuaEntity entityId, LuaEntity cameraId) -> bool {
            auto* pc = GetPC(world, entityId);
            if (!pc) return false;
            // BindAndStamp auto-adds a GuidComponent to the camera entity if
            // it doesn't already have one, so this Lua call survives the
            // scene save/load round-trip.
            const Entity cam = LuaId::ResolveLive(world, cameraId);
            if (cam == NullEntity) return false;
            pc->cameraEntity.BindAndStamp(world, cam);
            return true;
        });

    // Convenience: in one call, drop in CharacterController + Player + zero
    // out LocalTransform. Saves Lua callers from doing the three-add dance
    // for the common "create a player avatar at origin" pattern. Existing
    // components on the entity are NOT overwritten.
    pl.set_function("MakePlayer",
        [&world](LuaEntity entityId) -> bool {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            if (!world.HasComponent<LocalTransform>(e))
                world.AddComponent<LocalTransform>(e, LocalTransform{});
            if (!world.HasComponent<GlobalTransform>(e))
                world.AddComponent<GlobalTransform>(e, GlobalTransform{});
            if (!world.HasComponent<CharacterControllerComponent>(e))
                world.AddComponent<CharacterControllerComponent>(e, CharacterControllerComponent{});
            if (!world.HasComponent<PlayerComponent>(e))
                world.AddComponent<PlayerComponent>(e, PlayerComponent{});
            return true;
        });

    lua["Player"] = pl;

    // ---- Camera.* ----
    // Active-camera switching. App::RefreshMainCamera prefers the entity tagged with ActiveCameraTag;
    // SetActive strips the tag from any prior holder and pins it to the requested entity. Use it for:
    //   * editor preview cam <-> play cam swap
    //   * cutscene push (save current, SetActive new, restore on end)
    //   * debug fly-cam toggle
    //   * death-replay / spectator cam
    sol::object existingCam = lua["Camera"];
    sol::table cam = existingCam.is<sol::table>()
                   ? existingCam.as<sol::table>()
                   : lua.create_table();

    cam.set_function("SetActive",
        [&world](LuaEntity entityId) -> bool {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) {
                LOG_WARNING("Camera.SetActive: entity %u is not alive/live", entityId.raw());
                return false;
            }
            if (!world.HasComponent<CameraComponent>(e)) {
                LOG_WARNING("Camera.SetActive: entity %u has no CameraComponent", entityId.raw());
                return false;
            }
            // Strip the tag from any previous holder (at most one active camera). Collect first, then remove:
            // the pool must not be mutated inside ForEach.
            std::vector<Entity> prevHolders;
            world.ForEach<ActiveCameraTag>([&](Entity other, ActiveCameraTag&)
            {
                if (other != e) prevHolders.push_back(other);
            });
            for (Entity p : prevHolders)
                world.RemoveComponent<ActiveCameraTag>(p);

            if (!world.HasComponent<ActiveCameraTag>(e))
                world.AddComponent<ActiveCameraTag>(e, ActiveCameraTag{});
            return true;
        });

    // Returns the tagged active camera entity, or 0 (NullEntity) if no entity carries the tag. Without a
    // tag the renderer still uses a fallback camera; this getter reports only the explicit assignment.
    cam.set_function("GetActive",
        [&world]() -> LuaEntity {
            Entity active = NullEntity;
            world.ForEach<ActiveCameraTag>([&](Entity e, ActiveCameraTag&)
            {
                if (active == NullEntity) active = e;
            });
            return LuaId::OutMaybe(&world, active);
        });

    // Camera.ClearActive(): drop the tag without picking a new camera. App::RefreshMainCamera then keeps
    // its cached camera while it is valid, else the first camera found. Useful at the end of a cutscene
    // before SetActive on the gameplay cam, or when tearing down a scene.
    cam.set_function("ClearActive",
        [&world]() -> bool {
            std::vector<Entity> holders;
            world.ForEach<ActiveCameraTag>([&](Entity e, ActiveCameraTag&)
            {
                holders.push_back(e);
            });
            for (Entity h : holders)
                world.RemoveComponent<ActiveCameraTag>(h);
            return true;
        });

    // ---- Camera Stack (docs/design/camera_stack_system.md) ----
    // Camera.PushVCam / PopVCam / SetPriority / SetWeight / SetEnabled / HardCutTo / AddShake /
    // HashChannel.
    //
    // Cameras are pushed onto a per-channel stack; the highest-priority VCam wins, ties blend by weight.
    // PushVCam takes an optional table:
    //   { priority=100, weight=, blendIn=0.4, blendOut=0.3,
    //     curveIn="EaseOut", curveOut="EaseIn",
    //     channel="Main" or 0x...uint }
    auto resolveChannel = [&](const sol::object& chArg) -> CameraChannelId {
        if (chArg.is<std::string>())
            return Camera::HashChannel(chArg.as<std::string>().c_str());
        if (chArg.is<uint32_t>())
            return static_cast<CameraChannelId>(chArg.as<uint32_t>());
        return Camera::kMainChannel;
    };
    auto resolveCurve = [](const sol::object& c) -> BlendCurve {
        if (c.is<std::string>()) {
            const std::string s = c.as<std::string>();
            if (s == "Linear")    return BlendCurve::Linear;
            if (s == "EaseIn")    return BlendCurve::EaseIn;
            if (s == "EaseOut")   return BlendCurve::EaseOut;
            if (s == "EaseInOut" || s == "Smooth") return BlendCurve::EaseInOut;
        }
        return BlendCurve::EaseOut;
    };

    cam.set_function("HashChannel",
        [](const std::string& name) -> uint32_t {
            return Camera::HashChannel(name.c_str());
        });

    cam.set_function("PushVCam",
        [&world, resolveChannel, resolveCurve](LuaEntity entityId, sol::object optsObj) -> LuaEntity {
            // ResolveLive, not IsAlive: a stale id whose slot was recycled must
            // not push the NEW occupant onto the camera stack.
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) {
                // .raw(): entityId is a class type, and passing it to the C varargs of LOG_* with %u is
                // conditionally-supported UB.
                LOG_WARNING("Camera.PushVCam: entity %u not alive (or recycled)",
                            entityId.raw());
                return {};
            }
            Camera::PushVCamArgs args{};
            if (optsObj.is<sol::table>())
            {
                sol::table t = optsObj.as<sol::table>();
                args.priority         = t.get_or("priority",  args.priority);
                args.weight           = t.get_or("weight",    args.weight);
                args.blendInDuration  = t.get_or("blendIn",   args.blendInDuration);
                args.blendOutDuration = t.get_or("blendOut",  args.blendOutDuration);
                // Override only when the key is present: resolveCurve falls back to EaseOut on nil, which would
                // replace the struct's own defaults (curveOut is EaseIn).
                if (t["curveIn"].valid())  args.curveIn  = resolveCurve(t["curveIn"]);
                if (t["curveOut"].valid()) args.curveOut = resolveCurve(t["curveOut"]);
                args.channelId        = resolveChannel(t["channel"]);
            }
            Camera::PushVCam(world, e, args);
            // Re-encode through LuaId::Out so the returned handle keeps its generation bits and later Pop /
            // SetPriority calls can check it.
            return LuaId::Out(world, e);   // handle == entity id
        });

    cam.set_function("PopVCam",
        [&world, resolveCurve](LuaEntity entityId, sol::object optsObj) -> bool {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            float blendOut = -1.f;
            BlendCurve curve = BlendCurve::Custom; // sentinel = leave alone
            if (optsObj.is<sol::table>())
            {
                sol::table t = optsObj.as<sol::table>();
                blendOut = t.get_or("blendOut", -1.f);
                if (t["curveOut"].valid()) curve = resolveCurve(t["curveOut"]);
            }
            Camera::PopVCam(world, e, blendOut, curve);
            return true;
        });

    // ResolveLive in all three: a stale id would write priority / weight / enabled into whatever VCam
    // later recycled the slot ("the camera jumped to the wrong place for no reason").
    cam.set_function("SetPriority",
        [&world](LuaEntity entityId, int priority) -> bool {
            auto* prio = world.GetComponent<VCamPriorityComponent>(
                LuaId::ResolveLive(world, entityId));
            if (!prio) return false;
            prio->priority = priority;
            return true;
        });

    cam.set_function("SetWeight",
        [&world](LuaEntity entityId, float w) -> bool {
            auto* prio = world.GetComponent<VCamPriorityComponent>(
                LuaId::ResolveLive(world, entityId));
            if (!prio) return false;
            prio->weight = w;
            return true;
        });

    cam.set_function("SetEnabled",
        [&world](LuaEntity entityId, bool enabled) -> bool {
            auto* prio = world.GetComponent<VCamPriorityComponent>(
                LuaId::ResolveLive(world, entityId));
            if (!prio) return false;
            prio->enabled = enabled;
            return true;
        });

    cam.set_function("HardCutTo",
        [&world, resolveChannel](LuaEntity entityId, sol::object optsObj) -> bool {
            // Validate BEFORE cutting: Camera::HardCutTo raises the channel's requestHardCut flag, which drops
            // TAA / XeGTAO / SSR / volumetric history, so a cut for a dead entity would throw history away
            // without switching cameras. Return false so the script can tell.
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) {
                LOG_WARNING("Camera.HardCutTo: entity %u not alive (or recycled)",
                            entityId.raw());
                return false;
            }
            CameraChannelId ch = Camera::kMainChannel;
            if (optsObj.is<sol::table>())
                ch = resolveChannel(optsObj.as<sol::table>()["channel"]);
            Camera::HardCutTo(world, e, ch);
            return true;
        });

    cam.set_function("AddShake",
        [&world, resolveChannel](float trauma, sol::object optsObj) -> bool {
            CameraChannelId ch = Camera::kMainChannel;
            float falloff = 1.5f;
            if (optsObj.is<sol::table>()) {
                sol::table t = optsObj.as<sol::table>();
                ch = resolveChannel(t["channel"]);
                falloff = t.get_or("falloff", falloff);
            }
            const Entity channelEnt = Camera::GetOrCreateChannelEntity(world, ch);
            if (!world.HasComponent<CameraShakeComponent>(channelEnt))
                world.AddComponent<CameraShakeComponent>(channelEnt, CameraShakeComponent{});
            auto* shake = world.GetComponent<CameraShakeComponent>(channelEnt);
            if (!shake) return false;
            // Trauma stacks additively up to 1.0 (hits in the same frame layer); falloff drains at the most
            // recent rate.
            shake->trauma = std::min(1.f, shake->trauma + trauma);
            shake->falloffPerSec = falloff;
            return true;
        });

    lua["Camera"] = cam;
}

void RegisterLuaPlayerEngineBindings(sol::state& state, World& world, const Entity& cameraEntity)
{
    sol::table engineTbl = state["Engine"];

    // Engine.GetPlayer() -> entityId: the active player handle. Prefers the entity carrying
    // PlayerComponent (Player.MakePlayer), falling back to the main camera entity for camera-is-player
    // first-person setups; 0 if neither.
    engineTbl.set_function("GetPlayer",
        [&world, &cameraEntity]() -> LuaEntity {
            Entity found = NullEntity;
            world.ForEach<PlayerComponent>([&](Entity e, PlayerComponent&) {
                if (found == NullEntity) found = e;
            });
            if (found == NullEntity) found = cameraEntity;
            return LuaId::Out(world, found);
        });
}
