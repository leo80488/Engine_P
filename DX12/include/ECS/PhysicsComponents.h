#pragma once

// Physics components - plain data; no Jolt type leaks here. PhysicsSystem builds the matching
// Jolt body on first sight and afterwards writes LocalTransform back from the simulation.

#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>
#include <string>

// Mirrors JPH::BodyID::cInvalidBodyID (0xffffffff) so the header stays Jolt-free.
inline constexpr std::uint32_t kInvalidPhysicsBodyId = 0xffffffffu;

struct RigidBodyComponent : ComponentBase
{
    enum class Motion : std::uint8_t
    {
        Static,     // never moves; no mass
        Kinematic,  // moved by code, pushes dynamics
        Dynamic,    // full simulation (gravity, impulses, contact response)
    };

    enum AxisLock : std::uint8_t
    {
        LockTranslationX = 1 << 0,
        LockTranslationY = 1 << 1,
        LockTranslationZ = 1 << 2,
        LockRotationX    = 1 << 3,
        LockRotationY    = 1 << 4,
        LockRotationZ    = 1 << 5,
        LockTranslationAll = LockTranslationX | LockTranslationY | LockTranslationZ,
        LockRotationAll    = LockRotationX    | LockRotationY    | LockRotationZ,
    };

    Motion motion          = Motion::Dynamic;
    float  mass            = 1.0f;   // kg, used only for Dynamic
    float  linearDamping   = 0.05f;
    float  angularDamping  = 0.05f;
    float  friction        = 0.5f;
    float  restitution     = 0.0f;
    float  gravityFactor   = 1.0f;
    // Per-axis lock for Dynamic bodies; a set bit freezes that axis (zero linear / angular velocity
    // on it). Jolt's EAllowedDOFs inverted, because authoring reads as "freeze X". One byte, not six
    // bools, so the inspector and serializer round-trip one integer.
    std::uint8_t lockedAxes = 0;     // bitmask of AxisLock values; 0 = full 6-DOF

    // One-shot launch velocity (world space), applied once when the Jolt body is created, then
    // cleared: lets a script (Physics.SpawnBall) launch a freshly spawned dynamic body across the lazy
    // body-creation gap. PhysicsSystem also fills it when a replaced component (a deserialize over a
    // live body) takes over a moving body, so the rebuilt body keeps its velocity. Runtime only,
    // never serialized.
    DirectX::XMFLOAT3 initialVelocity        = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3 initialAngularVelocity = { 0.f, 0.f, 0.f };
    bool              hasInitialVelocity     = false;

    // Runtime, filled by PhysicsSystem on creation. Do not edit.
    std::uint32_t bodyId = kInvalidPhysicsBodyId;
    // Generation of the ColliderComponent the active body was built from; PreAllSteps rebuilds the
    // body when ColliderComponent::generation moves past it (MarkDirty, a re-bake). Runtime only.
    std::uint32_t lastBuiltGeneration = 0;
    // lockedAxes the active body was built from. Jolt applies DOF locks only at body creation
    // (MotionProperties::SetMassProperties with EAllowedDOFs) and BodyInterface cannot change them,
    // so a drift rebuilds the body.
    std::uint8_t  lastBuiltLockedAxes  = 0;

    // ---- Per-frame change detection (runtime only, never serialized) ----
    // PreAllSteps asks every body several times a frame whether anything changed. Keeping that state
    // on the component lets the loops that already walk the dense pool read it for free; entity-keyed
    // maps cost cache-missing hash lookups per body (~1.7 ms a frame on a ~7k-static-body level, more
    // than the Jolt solve).

    // Hash of the ColliderComponent fields baked into the active body (the shape inputs, isTrigger,
    // layer). Phase 0.6 of PreAllSteps rebuilds the body when it drifts, which catches Inspector writes
    // that bypass MarkDirty().
    std::uint64_t lastBuiltShapeHash = 0;

    // Hash of the mesh source path's CONTENT, kept out of lastBuiltShapeHash because hashing it reads
    // an out-of-line heap buffer (per body per frame, that dominated the collider diff on a level of
    // mesh colliders). Compared on a rotating 1/kPathHashSlices subset of bodies per frame; the path's
    // LENGTH stays in lastBuiltShapeHash, so only a same-length path edit waits for its slice.
    std::uint64_t lastBuiltPathHash = 0;

    // Motion type the active Jolt body was BUILT with. The collider diff compares this integer (not
    // GetMotionType, which takes a body lock) and rebuilds the body when `motion` drifts: a motion
    // change is never applied in place (see Phase 0.6 in PhysicsSystem::PreAllSteps).
    Motion        lastAppliedMotion  = Motion::Dynamic;
    bool          lastAppliedMotionValid = false;

    // Last world pose pushed to Jolt for a Static / Kinematic body. SetPositionAndRotation takes a
    // body lock and notifies the broadphase of an AABB change, so never-moving level geometry must not
    // pay it every frame: re-push only when the entity's transform actually differs.
    DirectX::XMFLOAT3 pushedPos       = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT4 pushedRot       = { 0.f, 0.f, 0.f, 1.f };
    bool              pushedPoseValid = false;

    // The GlobalTransform matrix that pose was derived from. Deriving it costs an XMMatrixDecompose
    // (~150 ns; at 7k static bodies most of the physics phase), so a 64-byte compare against the source
    // matrix skips it. Byte-exact on purpose, like TransformSystem's own cache: a value compare cannot
    // miss one of the ~40 transform writers, a dirty flag can. Trusted only while GlobalTransform is
    // authoritative (non-identity); otherwise the pose comes from the LocalTransform chain and this
    // stays invalid.
    DirectX::XMFLOAT4X4 poseSrcGlobal{};
    bool                poseSrcValid = false;

    // A Kinematic body PhysicsSystem::MoveKinematicBodies has given a velocity: it stops it when the body arrives,
    // because Jolt keeps integrating the velocity a kinematic body was last given. Runtime only.
    bool                kinematicDriven = false;

    bool IsAxisLocked(AxisLock a) const { return (lockedAxes & a) != 0; }
    void SetAxisLocked(AxisLock a, bool v)
    {
        if (v) lockedAxes |= a; else lockedAxes = static_cast<std::uint8_t>(lockedAxes & ~a);
    }
};

// A copy (Play / Stop's world snapshot, undo, a sequencer's restore, Component Transfer) owns no Jolt
// body: the runtime fields go back to "not built yet". Restored over an entity whose body was rebuilt
// since, a kept bodyId names a destroyed body - PhysicsSystem then builds none, writes that id's failed
// lookup (the origin) into LocalTransform every frame and later destroys the stale id again. Reset,
// Phase 0.5 reaps whatever body the entity has now and Phase 1 builds one from the copy. Constants
// only: World::AdoptState relies on every cloner being idempotent.
template <>
struct ComponentCloner<RigidBodyComponent>
{
    static RigidBodyComponent Clone(const RigidBodyComponent& src)
    {
        static const RigidBodyComponent kFresh{};
        RigidBodyComponent c = src;
        c.bodyId                 = kFresh.bodyId;
        c.lastBuiltGeneration    = kFresh.lastBuiltGeneration;
        c.lastBuiltLockedAxes    = kFresh.lastBuiltLockedAxes;
        c.lastBuiltShapeHash     = kFresh.lastBuiltShapeHash;
        c.lastBuiltPathHash      = kFresh.lastBuiltPathHash;
        c.lastAppliedMotion      = kFresh.lastAppliedMotion;
        c.lastAppliedMotionValid = kFresh.lastAppliedMotionValid;
        c.pushedPos              = kFresh.pushedPos;
        c.pushedRot              = kFresh.pushedRot;
        c.pushedPoseValid        = kFresh.pushedPoseValid;
        c.poseSrcGlobal          = kFresh.poseSrcGlobal;
        c.poseSrcValid           = kFresh.poseSrcValid;
        c.kinematicDriven        = kFresh.kinematicDriven;
        return c;
    }
};

struct ColliderComponent : ComponentBase
{
    // APPEND-ONLY: the raw value is serialized (`shape=%u` in .iscene / .iscn), so inserting in the
    // middle silently remaps every saved collider.
    // MeshAsset: a Mesh's asset collision - the body prims of its model's `BO` / `PR` records
    // (Resource/ModelCollision.h), built by PhysicsSystem as one shape (a StaticCompoundShape for
    // several). `assetModel` + `assetMeshKey` name it; an empty model = the StaticMesh on the same
    // entity. Static unless every prim is convex. An older binary that does not know the value builds
    // its unknown-shape fallback.
    enum class Shape : std::uint8_t { Box, Sphere, Capsule, Mesh, ConvexHull, MeshAsset };

    enum class CollisionLayer : std::uint8_t
    {
        Auto = 0,
        StaticEnv, DynamicProp, Character, Ragdoll,
        HurtBox, AttackBox, Projectile, Sensing,   // == Jolt layer index + 1
    };

    Shape             shape       = Shape::Box;
    DirectX::XMFLOAT3 halfExtents = { 0.5f, 0.5f, 0.5f }; // Box
    float             radius      = 0.5f;                  // Sphere / Capsule
    float             halfHeight  = 0.5f;                  // Capsule cylinder half-height (excl. hemispheres)

    // Geometry source for Mesh and ConvexHull. PhysicsSystem reads the .meshlib on first body
    // creation and caches the built shape by (path, meshId), so entities sharing an entry share one
    // Jolt shape.
    //   Mesh       -> JPH::MeshShape from the entry's triangles. STATIC ONLY (Jolt's MeshShape is
    //                 invalid for dynamic motion; PhysicsSystem coerces the motion to Static).
    //   ConvexHull -> JPH::ConvexHullShape from the entry's VERTICES (indices ignored). Works on
    //                 dynamic and kinematic bodies and queries far cheaper than a trimesh; an
    //                 imported Unreal UCX_ hull maps to it.
    std::string   meshLibPath;
    std::uint32_t meshLibMeshId = 0;

    // MeshAsset: the model (.iscn, project-relative) and the MeshKey in it.
    std::string   assetModel;
    std::uint64_t assetMeshKey = 0;

    // Local-space translation of the collision shape relative to the entity. Jolt primitives are
    // centred at the origin, so a capsule on a pivot-at-feet character is half buried: set
    // offset.y = halfHeight + radius (or use the inspector's "Snap to Bottom" preset). The primitive is
    // wrapped in a JPH::RotatedTranslatedShape when offset != 0 or rotation != identity.
    // Same units as halfExtents / radius / halfHeight - the entity's local space - so it scales and
    // mirrors with the entity's world scale like the primitive: a prefab authored at scale 1 keeps its
    // collider on the mesh at 0.1, and a leaf mirrored with sx=-1 gets its box mirrored too
    // (PhysicsSystem::MakeShape multiplies the offset by the decomposed world scale;
    // CollisionMeshBaker's wireframe does the same).
    DirectX::XMFLOAT3 offset = { 0.f, 0.f, 0.f };

    // Local-space rotation of the collision shape (XYZ Euler, degrees): a capsule on its side, a box
    // tilted to an angled mesh. Euler degrees rather than a quaternion, as for FollowSocket /
    // FollowEntity, so the inspector stays jitter-free near gimbal lock; PhysicsSystem converts to
    // JPH::Quat inside the RotatedTranslatedShape at body build.
    DirectX::XMFLOAT3 rotationEulerDeg = { 0.f, 0.f, 0.f };

    // Trigger / sensor: PhysicsSystem builds the body as a Jolt sensor (mIsSensor), which reports
    // contacts (OnEnter / OnExit reach Lua) with no collision response, so the volume neither blocks
    // nor pushes what enters it (door trigger zones, pickup volumes). Dynamic / Kinematic bodies are
    // detected by the contact listener, KCC characters (CharacterControllerComponent) by the per-frame
    // CollideShape sweep in PhysicsSystem::PostAllSteps; both publish the same ContactBegan /
    // ContactEnded events. Toggling it rebuilds the body. Serialized.
    bool isTrigger = false;

    // Collision-layer override (Unity Layer / Unreal Channel analogue). Auto (default): Static motion
    // -> StaticEnv, Dynamic / Kinematic -> DynamicProp, except triggers, which go to Sensing so
    // invisible volumes do not intercept gameplay raycasts (LAYER_MASK_DEFAULT excludes it). An
    // explicit value pins the body to one of the 8 gameplay layers; what collides with what is the
    // kCollidesWith matrix in PhysicsSystem.cpp (keep it symmetric). Changing this rebuilds the body.
    // Serialized; a missing key loads as Auto.
    CollisionLayer layer = CollisionLayer::Auto;

    // Bumped whenever the collider definition changes in a way that needs a new Jolt body (shape,
    // dimensions, mesh source). PhysicsSystem also diffs a fingerprint every frame to catch direct
    // field writes (the Inspector's reflection layer), so a script may call MarkDirty() or just
    // assign fields. Runtime only.
    std::uint32_t generation = 0;

    // Prewarm memo for PhysicsSystem::PreAllSteps, whose every-frame scan looks for Mesh / ConvexHull /
    // MeshAsset shapes missing from the Jolt shape cache. Building the cache key needs a std::string
    // (~14k heap allocations a frame on a level of 5k+ hull colliders), so non-zero here means "already
    // confirmed in the cache" and the scan skips the collider with one integer compare.
    // The value encodes (shape, meshLibMeshId, generation, meshLibPath.size(), assetMeshKey,
    // assetModel.size(), the shape-cache epoch), so MarkDirty(), a re-bake, a mesh-id swap or a
    // shape-cache clear invalidates it. A meshLibPath / assetModel edit that keeps the length, the
    // mesh id / key and generation slips through safely: the shape is then built lazily at body
    // creation (GetOrBuildMeshShape / GetOrBuildAssetShape) instead of by the batch prewarm. A
    // collider-diff rebuild also clears it. Runtime only.
    std::uint64_t shapePrewarmMemo = 0;

    void MarkDirty() { ++generation; }
};

// On an entity whose Collider + RigidBody were DERIVED from a StaticMesh's asset collision
// (MeshInstanceSystem): they are rebuilt from the asset and never serialized (the Collider /
// RigidBody serializers skip an entity with this tag). An explicit Collider the user adds replaces
// them. Runtime only, no serializer.
struct DerivedColliderTag {};

// What a model part was GIVEN at spawn: the Collider + RigidBody a .iscn C record or the model's
// asset collision put on it (SceneInstanceLoader). SaveScene writes an MC line only where the
// part's live components differ from these, so a collider the model itself gives is not saved as
// an override. Absent = the part was given nothing. Runtime only, no serializer (like
// MaterialSpawnDefault).
struct ColliderSpawnDefault
{
    ColliderComponent  collider;
    RigidBodyComponent rigidBody;
    // From the model's ASSET collision (not a C record): a StaticMesh of the same
    // Mesh derives it again (Unpack Model Instance leaves it to that).
    bool               fromAsset = false;
};
