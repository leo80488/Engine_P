#include "Physics/LuaPhysicsBindings.h"
#include "LuaPhysicsInternal.h"
#include "Physics/PhysicsSystem.h"
#include "Scripting/LuaDoc.h"
#include "ECS/NotifyTypes.h"          // PendingHitboxCommands (Hitbox.*)
#include "ECS/AnimationComponents.h"  // SocketComponent::HashName
#include "ECS/ProjectileComponent.h"  // Projectile.Spawn
#include "ECS/LifetimeComponent.h"
#include "ECS/HierarchyComponents.h"  // LocalTransform / GlobalTransform
#include <cmath>

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform
#include "ECS/PhysicsComponents.h"     // RigidBodyComponent / ColliderComponent
#include "Scene/MeshSpawner.h"         // MeshSpawner::Spawn
#include "Resource/ProceduralMesh.h"   // PrimitiveMeshType
#include "System/Log.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

#include <tuple>
#include <utility>

namespace DX12Physics
{
// Documented in LuaPhysicsInternal.h.
std::uint16_t ParseLayerMask(const sol::object& v, std::uint16_t fallback,
                             const char* whereForLog)
{
    auto one = [whereForLog](const std::string& n, bool* ok) -> std::uint16_t
    {
        *ok = true;
        if (n == "StaticEnv")   return MakeLayerMask(Layer::StaticEnv);
        if (n == "DynamicProp") return MakeLayerMask(Layer::DynamicProp);
        if (n == "Character")   return MakeLayerMask(Layer::Character);
        if (n == "Ragdoll")     return MakeLayerMask(Layer::Ragdoll);
        if (n == "HurtBox")     return MakeLayerMask(Layer::HurtBox);
        if (n == "AttackBox")   return MakeLayerMask(Layer::AttackBox);
        if (n == "Projectile")  return MakeLayerMask(Layer::Projectile);
        if (n == "Sensing")     return MakeLayerMask(Layer::Sensing);
        *ok = false;
        LOG_WARNING("%s: unknown layer '%s' (StaticEnv/DynamicProp/Character/"
                    "Ragdoll/HurtBox/AttackBox/Projectile/Sensing)",
                    whereForLog, n.c_str());
        return 0;
    };

    if (v.is<std::string>())
    {
        bool ok = false;
        const std::uint16_t m = one(v.as<std::string>(), &ok);
        return ok ? m : fallback;
    }
    if (v.is<sol::table>())
    {
        std::uint16_t acc = 0;
        for (auto& kv : v.as<sol::table>())
        {
            if (!kv.second.is<std::string>()) continue;
            bool ok = false;
            acc = static_cast<std::uint16_t>(acc | one(kv.second.as<std::string>(), &ok));
        }
        return acc != 0 ? acc : fallback;
    }
    if (v.is<int>())
        return static_cast<std::uint16_t>(v.as<int>());
    if (v.get_type() == sol::type::number)   // a float holding a mask (2^4 is a float in Lua 5.4)
    {
        const double d = v.as<double>();
        if (d >= 0.0 && d <= 65535.0) return static_cast<std::uint16_t>(d);
    }
    return fallback;
}

void RegisterLuaPhysicsBindings(sol::state& lua, PhysicsSystem& physics, World& world)
{
    sol::table tbl = lua.create_table();

    // Raycast, RayAll, SphereCast, BoxCast, CapsuleCast, OverlapSphere, OverlapBox, OverlapCapsule
    // (LuaPhysicsQueries.cpp).
    RegisterLuaPhysicsQueries(lua, tbl, physics, world);

    LuaDoc::Fn(tbl, "Physics", "BodyCount",
        [&physics]() -> int { return static_cast<int>(physics.GetBodyCount()); },
        { .doc = "The number of Jolt bodies alive now: rigid bodies, plus the inner body of a CharacterController with "
                 "queryBody. A body nothing owns is counted too, so a leaked ghost collider shows up here." });

    LuaDoc::Fn(tbl, "Physics", "ActiveBodyCount",
        [&physics]() -> int { return static_cast<int>(physics.GetActiveBodyCount()); },
        { .doc = "The number of rigid bodies awake now. A resting body falls asleep after physics.timeBeforeSleep "
                 "(engine.json, default 0.5 s) and stops counting; a touch from an awake body wakes it." });

    LuaDoc::Fn(tbl, "Physics", "IsSleeping",
        [&physics, &world](LuaEntity entity) -> bool
        {
            return physics.IsSleeping(LuaId::ResolveLive(world, entity));
        },
        { .doc = "True when the entity's dynamic body is asleep (at rest for physics.timeBeforeSleep seconds). A "
                 "sleeping body keeps its contacts: no ContactEnded fires for it.",
          .params = { { .name = "entity", .doc = "the entity that owns the body" } } });

    // Layer-mask constants for the optional mask arguments. The bits are disjoint powers of two, so
    // combine them with +, e.g. Physics.Raycast(..., Physics.Layers.StaticEnv + Physics.Layers.Sensing).
    {
        sol::table layers = lua.create_table();
        layers["StaticEnv"]   = (int)MakeLayerMask(Layer::StaticEnv);
        layers["DynamicProp"] = (int)MakeLayerMask(Layer::DynamicProp);
        layers["Character"]   = (int)MakeLayerMask(Layer::Character);
        layers["Ragdoll"]     = (int)MakeLayerMask(Layer::Ragdoll);
        layers["HurtBox"]     = (int)MakeLayerMask(Layer::HurtBox);
        layers["AttackBox"]   = (int)MakeLayerMask(Layer::AttackBox);
        layers["Projectile"]  = (int)MakeLayerMask(Layer::Projectile);
        layers["Sensing"]     = (int)MakeLayerMask(Layer::Sensing);
        tbl["Layers"]      = layers;
        tbl["MaskDefault"] = (int)LAYER_MASK_DEFAULT;
        tbl["MaskAll"]     = (int)LAYER_MASK_ALL;
    }

    // ---- Hitbox.* ----
    // The same mailbox (PendingHitboxCommands) an authored hitbox notify writes, for attacks that do
    // not come from a clip (a projectile detonation, a trap, an ability with no animation). A script
    // only states the intent; HitboxSystem runs the query and publishes HitboxHitEvent, and a combat
    // system decides whether it costs health.
    // Commands are consumed in TickPhase::BoneAttachment, so a Begin from a Logic script
    // (TickPhase::GameplayLogic) takes effect the same frame. The window stays open until End: it is a
    // state, not a one-shot.
    {
        sol::table hb = lua.create_table();

        auto ensureMailbox = [&world](Entity e) -> PendingHitboxCommands*
        {
            world.EnsurePool<PendingHitboxCommands>();
            if (auto* mb = world.GetComponent<PendingHitboxCommands>(e)) return mb;
            world.AddComponent<PendingHitboxCommands>(e, {});
            return world.GetComponent<PendingHitboxCommands>(e);
        };

        // Hitbox.Begin(e, slot, opts) -> bool
        //   opts: radius, halfHeight, damage, hitGroup, socket (string),
        //         offset = {x,y,z}, layer (string name or numeric mask)
        hb.set_function("Begin",
            [&world, ensureMailbox](LuaEntity entityId, int slot,
                                    sol::optional<sol::table> optsObj) -> bool
            {
                const Entity e = LuaId::ResolveLive(world, entityId);
                if (e == NullEntity) return false;
                if (slot < 0) return false;
                auto* mb = ensureMailbox(e);
                if (!mb) return false;

                PendingHitboxCommands::Cmd cmd;
                cmd.type = HitboxCmdType::Begin;
                cmd.slot = static_cast<uint32_t>(slot);
                if (optsObj)
                {
                    sol::table o = *optsObj;
                    cmd.params.radius     = o.get_or("radius",     0.5f);
                    cmd.params.halfHeight = o.get_or("halfHeight", 0.f);
                    cmd.params.damage     = o.get_or("damage",    10.f);
                    cmd.params.hitGroup   = static_cast<uint32_t>(o.get_or("hitGroup", 0));
                    cmd.params.shape      = cmd.params.halfHeight > 0.f
                                          ? HitboxShapeKind::Capsule
                                          : HitboxShapeKind::Sphere;

                    // `owner`: the character responsible, for volumes that are not the character (projectiles,
                    // lingering AoE).
                    if (sol::object ow = o["owner"]; ow.valid())
                    {
                        const Entity inst = LuaId::ResolveLive(
                            world, ow.as<LuaEntity>());
                        cmd.params.instigator = inst;
                    }

                    cmd.params.requireTag = o.get_or("requireTag", std::string{});
                    cmd.params.ignoreTag  = o.get_or("ignoreTag",  std::string{});

                    const std::string socket = o.get_or("socket", std::string{});
                    cmd.params.socketHash = socket.empty()
                                          ? 0u : SocketComponent::HashName(socket.c_str());

                    if (sol::object off = o["offset"]; off.is<sol::table>())
                    {
                        sol::table t = off.as<sol::table>();
                        cmd.params.localOffset = {
                            t.get_or("x", 0.f), t.get_or("y", 0.f), t.get_or("z", 0.f) };
                    }

                    // A name, a list of names or a raw mask (ParseLayerMask). 0 keeps HitboxSystem's default
                    // (HurtBox).
                    cmd.params.layerMask = ParseLayerMask(o["layer"], 0, "Hitbox.Begin");
                }
                mb->commands.push_back(std::move(cmd));
                return true;
            });

        // Hitbox.End(e, slot) -> bool. Closing an already-closed slot is a harmless no-op, so a script
        // can call this unconditionally on exit.
        hb.set_function("End",
            [&world, ensureMailbox](LuaEntity entityId, int slot) -> bool
            {
                const Entity e = LuaId::ResolveLive(world, entityId);
                if (e == NullEntity || slot < 0) return false;
                auto* mb = ensureMailbox(e);
                if (!mb) return false;
                PendingHitboxCommands::Cmd cmd;
                cmd.type = HitboxCmdType::End;
                cmd.slot = static_cast<uint32_t>(slot);
                mb->commands.push_back(std::move(cmd));
                return true;
            });

        lua["Hitbox"] = hb;
    }

    // ---- Projectile.* ----
    // One call, because the pieces are useless apart: a projectile is a transform + a mover + a
    // lifetime + a damage volume, and any one missing gives a thing that silently does nothing
    // (Engine.CreateEntity() alone has NO LocalTransform, so a hand-rolled projectile sits at the
    // world origin). The damage volume is the same HitboxSystem a sword swing uses (sub-stepping,
    // hitGroup dedupe, self-exclusion, tag filtering): there is no second damage path.
    {
        sol::table pr = lua.create_table();

        // Projectile.Spawn(opts) -> entityId (0 on failure)
        //   position   x, y, z
        //   direction  dx, dy, dz  (normalised here) + speed
        //   gravity    m/s^2, negative falls. 0 (default) = straight line.
        //   life       seconds before it expires on its own
        //   radius     sweep radius for WORLD blocking
        //   block      layer name/mask that stops it (default StaticEnv+DynamicProp)
        //   damage, hitRadius, hitGroup, layer   -- the damage volume
        //   owner      the character to credit, and to never hit
        //   requireTag / ignoreTag               -- affiliation filter
        pr.set_function("Spawn",
            [&world](sol::table o) -> LuaEntity
            {
                const float speed = o.get_or("speed", 10.f);
                float dx = o.get_or("dx", 0.f);
                float dy = o.get_or("dy", 0.f);
                float dz = o.get_or("dz", 1.f);
                const float lenSq = dx*dx + dy*dy + dz*dz;
                if (lenSq <= 1e-8f)
                {
                    LOG_WARNING("Projectile.Spawn: direction is zero");
                    return LuaEntity{};
                }
                const float inv = 1.f / std::sqrt(lenSq);
                dx *= inv; dy *= inv; dz *= inv;

                const Entity e = world.CreateEntity();
                if (e == NullEntity) return LuaEntity{};

                LocalTransform lt;
                lt.translation = { o.get_or("x", 0.f), o.get_or("y", 0.f), o.get_or("z", 0.f) };
                world.AddComponent<LocalTransform>(e, lt);
                world.AddComponent<GlobalTransform>(e, {});

                ProjectileComponent pc;
                pc.velocity     = { dx * speed, dy * speed, dz * speed };
                pc.gravity      = o.get_or("gravity", 0.f);
                pc.radius       = o.get_or("radius", 0.1f);
                pc.faceVelocity = o.get_or("faceVelocity", true);
                pc.stopOnImpact = o.get_or("stopOnImpact", true);
                // What STOPS it. 0 keeps ProjectileSystem's default (StaticEnv | DynamicProp).
                pc.blockMask = ParseLayerMask(o["block"], 0, "Projectile.Spawn(block)");
                world.AddComponent<ProjectileComponent>(e, pc);

                // Always given a lifetime: a projectile that never expires and never hits anything is a leak that
                // looks like a gameplay bug.
                LifetimeComponent life;
                life.total = life.remaining = o.get_or("life", 5.f);
                world.AddComponent<LifetimeComponent>(e, life);

                // Damage volume: opened once, held for the whole flight.
                const float dmg = o.get_or("damage", 0.f);
                if (dmg > 0.f)
                {
                    PendingHitboxCommands mb;
                    PendingHitboxCommands::Cmd cmd;
                    cmd.type   = HitboxCmdType::Begin;
                    cmd.slot   = 0;
                    cmd.params.radius   = o.get_or("hitRadius", (std::max)(0.2f, pc.radius));
                    cmd.params.damage   = dmg;
                    cmd.params.hitGroup = static_cast<uint32_t>(o.get_or("hitGroup", 1));
                    cmd.params.requireTag = o.get_or("requireTag", std::string{});
                    cmd.params.ignoreTag  = o.get_or("ignoreTag",  std::string{});
                    if (sol::object ow = o["owner"]; ow.valid())
                        cmd.params.instigator = LuaId::ResolveLive(world, ow.as<LuaEntity>());

                    cmd.params.layerMask = ParseLayerMask(
                        o["layer"], MakeLayerMask(Layer::HurtBox), "Projectile.Spawn(layer)");

                    mb.commands.push_back(std::move(cmd));
                    world.EnsurePool<PendingHitboxCommands>();
                    world.AddComponent<PendingHitboxCommands>(e, std::move(mb));
                }

                return LuaId::Out(world, e);
            });

        lua["Projectile"] = pr;
    }

    // Physics.SpawnBall(x,y,z, radius, vx,vy,vz) -> entityId
    // A VISIBLE dynamic sphere with a sphere collider, launched along (vx,vy,vz). PhysicsSystem creates
    // the Jolt body lazily on its next tick (Phase 1) and applies the launch velocity, carried on
    // RigidBodyComponent, at that point.
    LuaDoc::Fn(tbl, "Physics", "SpawnBall",
        [&world](float x, float y, float z,
                 float radius,
                 float vx, float vy, float vz) -> LuaEntity
        {
            if (radius <= 0.f) radius = 0.25f;

            // Visual: the pre-uploaded GPU primitive sphere (base radius 0.5)
            // + default material + all render components (MeshSpawner).
            Entity e = MeshSpawner::Spawn(
                static_cast<int>(PrimitiveMeshType::Sphere), world);
            if (e == NullEntity) return {};

            // The primitive sphere has base radius 0.5 and MakeShape scales the collider by the entity's world
            // scale, so one scale of 2 x radius sizes both the mesh and the physics sphere to @p radius.
            if (auto* lt = world.GetComponent<LocalTransform>(e))
            {
                lt->translation = { x, y, z };
                lt->scale       = { radius * 2.f, radius * 2.f, radius * 2.f };
            }

            // Sphere collider at the primitive's base radius (0.5); the entity
            // scale above sizes it to `radius`.
            ColliderComponent col;
            col.shape  = ColliderComponent::Shape::Sphere;
            col.radius = 0.5f;
            world.AddComponent<ColliderComponent>(e, std::move(col));

            // Dynamic body carrying the one-shot launch velocity.
            RigidBodyComponent rb;
            rb.motion             = RigidBodyComponent::Motion::Dynamic;
            rb.mass               = 1.0f;
            rb.initialVelocity    = { vx, vy, vz };
            rb.hasInitialVelocity = true;
            world.AddComponent<RigidBodyComponent>(e, std::move(rb));

            return LuaId::Out(world, e);
        },
        { .doc = "Spawns a visible dynamic ball (primitive sphere mesh, sphere collider, mass 1) and launches it along "
                 "(vx, vy, vz). The Jolt body is built on the next physics tick and takes the launch velocity then; "
                 "nothing reclaims the ball, so destroy it yourself. radius <= 0 becomes 0.25. Returns 0 on failure.",
          .params = { { .name = "x", .doc = "position x" }, { .name = "y", .doc = "position y" },
                      { .name = "z", .doc = "position z" }, { .name = "radius", .doc = "ball radius" },
                      { .name = "vx", .doc = "launch velocity x" }, { .name = "vy", .doc = "launch velocity y" },
                      { .name = "vz", .doc = "launch velocity z" } },
          .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Update });

    lua["Physics"] = tbl;
    LOG_INFO("Physics: registered Lua bindings");
}

} // namespace DX12Physics
