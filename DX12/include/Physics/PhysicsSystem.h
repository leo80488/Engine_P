#pragma once

// PhysicsSystem - ECS bridge for Jolt Physics, stepped at a fixed 60 Hz.
// Creates Jolt bodies lazily for entities with both RigidBodyComponent and ColliderComponent, and
// writes Dynamic body poses and character-controller positions back into LocalTransform once per frame.
// Required per-frame order: scripts / game logic -> physics (PreAllSteps / StepOnce x N /
// PostAllSteps) -> TransformSystem::Propagate -> ApplyRenderInterpolation.
// Jolt types stay behind the pImpl, so this header is cheap to include.

#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>
#include "ECS/ECS.h"   // Entity for RayHit.entity

class World;
class DebugWirePass;
struct ColliderComponent;   // defined in ECS/PhysicsComponents.h

namespace JPH
{
    class CharacterVirtual;
    class PhysicsSystem;
}

namespace DX12Physics
{
    class PhysicsDebugDraw;

    // Result of a single-hit ray query. On a miss only `hit` is meaningful and every other field keeps
    // its default below, so callers can use the struct unconditionally.
    struct RayHit
    {
        bool              hit        = false;
        DirectX::XMFLOAT3 point      = { 0.f, 0.f, 0.f };
        DirectX::XMFLOAT3 normal     = { 0.f, 1.f, 0.f };
        float             distance   = 0.f;
        Entity            entity     = NullEntity;
        std::uint32_t     generation = 0;                 // the entity's generation when its body was built
        std::uint32_t     bodyId     = 0xffffffffu;
    };

    // One body from an exact (narrow-phase) shape-vs-shape overlap, with the contact it found.
    // OverlapSphere, by contrast, reports broad-phase AABB touches only.
    struct OverlapHit
    {
        Entity            entity = NullEntity;
        DirectX::XMFLOAT3 point  = { 0.f, 0.f, 0.f };   // on the hit body
        DirectX::XMFLOAT3 normal = { 0.f, 1.f, 0.f };   // points back at the query shape
        float             depth  = 0.f;                 // penetration depth
        std::uint32_t     bodyId = 0xffffffffu;
        std::uint32_t     generation = 0;               // the entity's generation when its body was built
    };

    // Result of a single-hit shape sweep. Like RayHit plus `fraction` (0..1 along the sweep) and
    // `bodyId`, so a later cast can skip a body (e.g. the character's own during collide-and-slide).
    struct ShapeCastHit
    {
        bool              hit      = false;
        DirectX::XMFLOAT3 point    = { 0.f, 0.f, 0.f };
        DirectX::XMFLOAT3 normal   = { 0.f, 1.f, 0.f };
        float             distance = 0.f;
        float             fraction = 0.f;
        Entity            entity   = NullEntity;
        std::uint32_t     bodyId   = 0xffffffffu;
        std::uint32_t     generation = 0;               // the entity's generation when its body was built
    };

    // What PhysicsSystem::DrawDebug drew.
    struct DebugDrawStats
    {
        std::uint32_t bodies     = 0;   // Jolt bodies drawn
        std::uint32_t asleep     = 0;   // ...of them dynamic and asleep
        std::uint32_t characters = 0;   // character-controller capsules drawn
    };

    // Public object layers, so the KCC and queries can choose what they hit (a ground probe takes
    // StaticEnv + DynamicProp and skips HurtBox / AttackBox / Sensing). Values must match the Jolt
    // ObjectLayer constants (Layers::) in PhysicsSystem.cpp.
    enum class Layer : std::uint8_t
    {
        StaticEnv   = 0,
        DynamicProp = 1,
        Character   = 2,
        Ragdoll     = 3,
        HurtBox     = 4,
        AttackBox   = 5,
        Projectile  = 6,
        Sensing     = 7,
    };

    // LayerMask: bit N set includes Layer N in a query. LAYER_MASK_DEFAULT covers the physical layers
    // and excludes the query-only ones (HurtBox, AttackBox, Sensing).
    using LayerMask = std::uint16_t;
    inline constexpr LayerMask MakeLayerMask(Layer l) { return LayerMask(1u << static_cast<std::uint8_t>(l)); }
    inline constexpr LayerMask LAYER_MASK_ALL     = LayerMask(0xFFFFu);
    inline constexpr LayerMask LAYER_MASK_DEFAULT =
        LayerMask((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6));   // StaticEnv | DynamicProp | Character | Ragdoll | Projectile

    // Camera spring-arm / wall probes: world geometry only. Character, Ragdoll and Projectile are
    // dropped because moving actors must not shove the camera, and because a CharacterControllerComponent
    // with queryBody set has an inner body on the CHARACTER layer that the orbit focus point sits inside:
    // under LAYER_MASK_DEFAULT the sweep hits it at fraction 0 and pins the camera at its minimum distance.
    inline constexpr LayerMask LAYER_MASK_CAMERA_PROBE =
        LayerMask((1u << 0) | (1u << 1));                                       // StaticEnv | DynamicProp

    // What a query does with trigger volumes (bodies built as sensors, ColliderComponent::isTrigger).
    enum class TriggerMode : std::uint8_t
    {
        Include,   // a sensor is hit like any other body
        Exclude,   // sensors are skipped
        Only,      // only sensors are hit
    };

    // What a query may skip. The layer mask decides first: a trigger on the Auto layer lives on Sensing, which
    // LAYER_MASK_DEFAULT leaves out, so it takes triggers = Include with a mask that names Sensing to see one.
    struct QueryFilter
    {
        LayerMask     layerMask    = LAYER_MASK_DEFAULT;
        TriggerMode   triggers     = TriggerMode::Include;
        std::uint32_t ignoreBodyId = 0xffffffffu;   // one body (a packed JPH::BodyID, e.g. ShapeCastHit::bodyId)
        Entity        ignoreEntity = NullEntity;    // every body of one entity: its rigid body and its query body
    };

    class PhysicsSystem
    {
    public:
        // One collider shape for PrewarmMeshShapes: the source file, the entry in it, and the shape kind.
        struct MeshShapeRequest { std::string path; uint32_t meshId; bool convexHull = false; };

        PhysicsSystem();
        ~PhysicsSystem();

        PhysicsSystem(const PhysicsSystem&)            = delete;
        PhysicsSystem& operator=(const PhysicsSystem&) = delete;

        // Registers the Jolt factory and types, starts the job pool and creates the JPH::PhysicsSystem.
        // Call once per process: scenes share the Jolt factory state.
        void Init();
        void Shutdown();

        // Destroys every Jolt body and all per-entity physics state before a scene reload (one
        // PhysicsSystem serves every scene). World::Clear recycles entity ids without firing destroy
        // listeners, so the per-entity reaper would miss bodies on reused ids: ghost colliders and body-pool
        // exhaustion. Call BEFORE the World is cleared, on the main thread, outside a physics step.
        // @p dropShapeCache: true for a real scene change. Pass false on Play -> Stop, which restores the
        // same scene: keeping the mesh / hull shapes avoids a ~39 ms hitch per Play press, and MeshShapeKey
        // folds the source file's modification stamp into the key, so a re-baked .meshlib is never stale.
        void OnWorldClear(bool dropShapeCache = true);

        // ---- Caller-driven fixed loop: PreAllSteps, StepOnce x N, PostAllSteps ----
        // App::RunFixedPhysicsLoop owns the accumulator, so the FixedPhysicsPre / Post phases run around
        // each substep.

        // Once per frame, before the substeps: prewarms uncached mesh shapes, reaps orphaned bodies, rebuilds
        // bodies whose collider changed, creates missing bodies and character controllers, pushes Static /
        // Kinematic poses to Jolt, and treats a Dynamic LocalTransform that differs from Jolt as a teleport.
        // @p editSync: Edit mode's pass (SyncEditMode): everything above except the character controllers.
        void PreAllSteps(World& world, bool editSync = false);

        // Edit mode: no step runs, but the bodies exist and follow the edited poses (Phase 2), so a ray, the
        // editor's drop-to-floor, the camera's wall probe and IK grounding find real geometry. Publishes no
        // event. Call once a frame instead of PreAllSteps / StepOnce / PostAllSteps.
        void SyncEditMode(World& world) { PreAllSteps(world, true); }

        // One fixed-dt substep: moves the current pose snapshot to previous, ticks the character controllers,
        // steps Jolt and captures the new current pose. Call once per GetFixedDt() in the caller's
        // accumulator.
        void StepOnce(World& world);

        // Once per frame, after the substeps: publishes the buffered contact events and the
        // character-vs-sensor ones (every ContactBegan before any ContactEnded) and writes Dynamic body poses
        // and character-controller positions back into LocalTransform. The events stay on the EventBus until
        // Gameplay/EventDelivery drains it (the end of BoneAttachment), in publish order with every other C++
        // event: no Lua runs inside the physics phase.
        void PostAllSteps(World& world);

        // FixedPhysicsPre, once per substep: moves every Kinematic body toward the pose its LocalTransform chain
        // gives (the previous frame's GlobalTransform for a body that follows a socket or another entity), with
        // MoveKinematic, so the body carries the velocity of the move: a character standing on it is carried, a
        // dynamic body in its way is pushed at its speed. The motion of the frame is spread over the frame's
        // substeps (SetFrameSubsteps). A body that is further than 5 m (or 1.5 rad) from its target is placed
        // there instead of driven. Edit mode and a body's first pose are PreAllSteps' (a teleport).
        void MoveKinematicBodies(World& world);

        // How many substeps the coming frame will run: App counts them from its accumulator before the loop, so
        // MoveKinematicBodies can divide the frame's motion evenly. 1 when never set.
        void SetFrameSubsteps(unsigned count) { m_frameSubsteps = count; }

        // Fixed physics step in seconds (60 Hz).
        static constexpr float GetFixedDt() noexcept { return 1.0f / 60.0f; }

        // Render-only interpolation: run AFTER TransformSystem::Propagate and BEFORE rendering. Overwrites
        // each Dynamic body's GlobalTransform.matrix with a pose lerped / slerped between its pre-step and
        // post-step snapshots, and only the translation of each character controller, then re-propagates
        // their children. @p renderAlpha is accumulator / fixedDt (FrameContext::physicsAlpha), clamped to
        // [0,1]. LocalTransform keeps Jolt's instantaneous pose, which PreAllSteps' teleport detection relies on.
        void ApplyRenderInterpolation(World& world, float renderAlpha);

        // Drops the mesh-shape and .meshlib blob caches and bumps `generation` on every Mesh / ConvexHull /
        // MeshAsset collider, so PreAllSteps rebuilds those bodies on the next tick from the fresh files.
        // Call after re-baking a collision .meshlib: without the bump, existing bodies keep their discarded
        // shapes indefinitely.
        void InvalidateMeshShapeCache(World& world);

        // Builds (and caches) the shapes of the uncached requests in parallel on TaskSystem, reading each
        // source file (.meshlib / .imsh) once per shape kind; blocks until done. PreAllSteps calls it for
        // colliders not cached yet; otherwise lazy body creation builds the shapes one at a time on the main
        // thread (seconds of freeze for 1k+ entities). A request's convexHull picks JPH::MeshShape from the
        // entry's triangles (false) or JPH::ConvexHullShape from its vertices (true); they cache under
        // different keys, so one entry may be requested both ways.
        void PrewarmMeshShapes(const std::vector<MeshShapeRequest>& requests);

        // Closest hit along a ray against every body - Static, Kinematic and Dynamic (Kinematic bodies are
        // hit too, as ground / wall probes want). @p direction need not be normalised; hit.distance is in
        // [0, maxDistance] along the normalised direction. Returns hit = false on a miss, before Init, or for
        // a zero @p direction or @p maxDistance <= 0. The default @p layerMask skips HurtBox / AttackBox /
        // Sensing (hitboxes, trigger volumes); pass LAYER_MASK_ALL to hit everything.
        RayHit CastRayClosest(const DirectX::XMFLOAT3& from,
                              const DirectX::XMFLOAT3& direction,
                              float                    maxDistance,
                              LayerMask                layerMask = LAYER_MASK_DEFAULT) const;

        // As above with a full QueryFilter (layers, trigger switch, a body or an entity to skip). Every query
        // that takes one also hands back the generation its entity had when the body was built, and walks only
        // the broad-phase trees the mask can reach.
        RayHit CastRayClosest(const DirectX::XMFLOAT3& from,
                              const DirectX::XMFLOAT3& direction,
                              float                    maxDistance,
                              const QueryFilter&       filter) const;

        // Every body along a ray, nearest first, one RayHit per body (a mesh or a compound reports its nearest
        // sub-shape). Appends to @p out without clearing, at most @p maxResults (the nearest ones); returns the
        // number appended.
        std::size_t CastRayAll(const DirectX::XMFLOAT3& from,
                               const DirectX::XMFLOAT3& direction,
                               float                    maxDistance,
                               std::vector<RayHit>&     out,
                               const QueryFilter&       filter     = {},
                               std::size_t              maxResults = 64) const;

        // Appends every entity whose body overlaps a sphere: the plural query perception needs (the other
        // queries return one closest hit). BROAD PHASE: it tests body AABBs, not shapes (far cheaper per agent
        // than a narrow-phase overlap), so it can return bodies just outside the radius; callers re-test
        // distance / FOV. Do NOT use it where an exact answer matters (damage, triggers): OverlapCapsuleAll is
        // exact. Appends to @p out without clearing, at most @p maxResults, in Jolt body order (not by
        // distance); returns the number appended. Bodies with no entity stamped are skipped.
        std::size_t OverlapSphere(const DirectX::XMFLOAT3& center,
                                  float                    radius,
                                  std::vector<Entity>&     out,
                                  LayerMask                layerMask  = LAYER_MASK_DEFAULT,
                                  std::size_t              maxResults = 256) const;

        // The same broad-phase query with a QueryFilter, handing back handles that carry the generation.
        std::size_t OverlapSphere(const DirectX::XMFLOAT3&   center,
                                  float                      radius,
                                  std::vector<EntityHandle>& out,
                                  const QueryFilter&         filter,
                                  std::size_t                maxResults = 256) const;

        // Exact, all-hits capsule overlap (Jolt NarrowPhaseQuery::CollideShape) for melee hit detection: a
        // hit means the shapes really overlap, and carries a contact point and normal. Do not use
        // OverlapSphere for damage: it is AABB-only and gives phantom hits and misses.
        // @p halfHeight <= 0 builds a sphere (Jolt asserts on a degenerate capsule); @p rotation orients the
        // capsule's local-Y segment. Appends to @p out without clearing, at most @p maxResults; returns the
        // number appended. Only the deepest sub-shape hit per entity is kept: one entry per entity.
        std::size_t OverlapCapsuleAll(const DirectX::XMFLOAT3&  center,
                                      const DirectX::XMFLOAT4&  rotation,   // quaternion
                                      float                     radius,
                                      float                     halfHeight,
                                      std::vector<OverlapHit>&  out,
                                      LayerMask                 layerMask  = LAYER_MASK_DEFAULT,
                                      std::size_t               maxResults = 64) const;

        // The same exact overlap with a QueryFilter.
        std::size_t OverlapCapsuleAll(const DirectX::XMFLOAT3&  center,
                                      const DirectX::XMFLOAT4&  rotation,
                                      float                     radius,
                                      float                     halfHeight,
                                      std::vector<OverlapHit>&  out,
                                      const QueryFilter&        filter,
                                      std::size_t               maxResults = 64) const;

        // Exact, all-hits box overlap (a box of @p halfExtents, oriented by @p rotation, a quaternion): the same
        // contract as OverlapCapsuleAll, one entry per entity with its deepest contact.
        std::size_t OverlapBoxAll(const DirectX::XMFLOAT3&  center,
                                  const DirectX::XMFLOAT4&  rotation,
                                  const DirectX::XMFLOAT3&  halfExtents,
                                  std::vector<OverlapHit>&  out,
                                  const QueryFilter&        filter     = {},
                                  std::size_t               maxResults = 64) const;

        // Animation-driven melee query: tests exact overlaps of the collider's authored Box / Sphere / Capsule
        // (offset, rotation and scale included) at poses sampled from the @p previous to the @p current world
        // matrix over [fromFraction, toFraction], lerping translation and slerping rotation, and appends the
        // contacts. No rigid body is needed. Deduplicated per sampled pose only, so an entity can appear more
        // than once. Returns the number appended; 0 for any other shape kind.
        std::size_t SweepColliderAll(const ColliderComponent& collider,
                                     const DirectX::XMFLOAT4X4& previous,
                                     const DirectX::XMFLOAT4X4& current,
                                     std::vector<OverlapHit>& out,
                                     LayerMask layerMask = LAYER_MASK_DEFAULT,
                                     float fromFraction=0.f, float toFraction=1.f) const;

        // Closest hit of a capsule sweep. @p halfHeight is the central segment's half length (hemispheres
        // excluded), @p radius the capsule radius; @p direction need not be normalised. @p ignoreBodyId
        // skips one body (the character's own during ground probes or collide-and-slide). The default
        // @p layerMask skips HurtBox / AttackBox / Sensing, so a movement probe does not catch hitboxes.
        ShapeCastHit CastCapsuleClosest(const DirectX::XMFLOAT3& from,
                                        const DirectX::XMFLOAT3& direction,
                                        float                    radius,
                                        float                    halfHeight,
                                        float                    maxDistance,
                                        std::uint32_t            ignoreBodyId = 0xffffffffu,
                                        LayerMask                layerMask    = LAYER_MASK_DEFAULT) const;

        // Closest hit of a capsule whose segment runs along the local Y axis rotated by @p rotation (a
        // quaternion), swept from @p from; @p halfHeight <= 0 sweeps a sphere.
        ShapeCastHit CastCapsuleClosest(const DirectX::XMFLOAT3& from,
                                        const DirectX::XMFLOAT4& rotation,
                                        const DirectX::XMFLOAT3& direction,
                                        float                    radius,
                                        float                    halfHeight,
                                        float                    maxDistance,
                                        const QueryFilter&       filter) const;

        // Closest hit of a box of @p halfExtents oriented by @p rotation (a quaternion), swept from @p from.
        ShapeCastHit CastBoxClosest(const DirectX::XMFLOAT3& from,
                                    const DirectX::XMFLOAT4& rotation,
                                    const DirectX::XMFLOAT3& halfExtents,
                                    const DirectX::XMFLOAT3& direction,
                                    float                    maxDistance,
                                    const QueryFilter&       filter = {}) const;

        // Closest hit of a sphere sweep, through the same cast path as CastCapsuleClosest. Cheaper than a
        // capsule of equal radius; suits e.g. a ledge probe.
        ShapeCastHit CastSphereClosest(const DirectX::XMFLOAT3& from,
                                       const DirectX::XMFLOAT3& direction,
                                       float                    radius,
                                       float                    maxDistance,
                                       std::uint32_t            ignoreBodyId = 0xffffffffu,
                                       LayerMask                layerMask    = LAYER_MASK_DEFAULT) const;

        // Returns the raw .meshlib bytes for a collider source path, loading them into the blob cache that
        // shape building uses, so body creation, debug views and other CPU readers share one copy. nullptr
        // before Init, for an empty path, or when the file cannot be read from disk or pak. The pointer is
        // valid until the blob cache is dropped: InvalidateMeshShapeCache, any OnWorldClear, or destruction.
        const std::vector<std::uint8_t>* EnsureCollisionBlob(const std::string& path);

        // Appends the collider's world-space triangles to @p outPositions / @p outIndices: the geometry Jolt
        // collides against, for every shape kind. They come from the JPH::Shape built by the same MakeShape()
        // as the body (GetTrianglesStart/Next), offset, rotation and scale included, so consumers cannot
        // disagree with the simulation. This is the only correct way to get a ConvexHull's surface: the
        // indices stored beside hull points in a .meshlib are a synthetic fan, not the hull.
        // Shapes come from the shared cache (N instances of one mesh, one build). Returns false when the
        // shape cannot be built or yields no triangles.
        bool AppendColliderTriangles(const ColliderComponent&        collider,
                                     const DirectX::XMMATRIX&        worldMatrix,
                                     std::vector<DirectX::XMFLOAT3>& outPositions,
                                     std::vector<std::uint32_t>&     outIndices);

        // Live Jolt body count (0 before Init), character-controller inner bodies included. Diagnostics:
        // Lua Physics.BodyCount, and the chain_preview_isolation pack, which asserts it is the same before,
        // while and after a Chain Physics Preview panel is open.
        std::uint32_t GetBodyCount() const;

        // Rigid bodies awake now (0 before Init): a resting body falls asleep after physics.timeBeforeSleep.
        // Lua Physics.ActiveBodyCount.
        std::uint32_t GetActiveBodyCount() const;

        // True when @p entity owns a Dynamic body that is asleep now. Lua Physics.IsSleeping.
        bool IsSleeping(Entity entity) const;

        // engine.json's physics.gravity and physics.timeBeforeSleep (or their -set:), handed in by the
        // owner after Init - App reads them through Resource::PeekEngineDevPhysics. The core does not read
        // the settings layer itself: that pulls the renderer into every unit test that links it. Without
        // the call the world keeps Jolt's defaults (-9.81, 0.5 s), which are the rows' defaults too.
        void ApplyWorldSettings(float gravity, float timeBeforeSleep);

#ifdef JPH_DEBUG_RENDERER
        // Draws every Jolt body (wireframe, coloured by sleep state: static grey, awake dynamic yellow,
        // asleep red, sensors yellow) and each character controller's capsule (green on the ground, cyan in
        // the air) into @p wire, through Jolt's debug renderer. @p cameraPos picks the level of detail.
        // The returned bodies equal GetBodyCount(): the harness check logs both. Not in Retail, whose Jolt
        // has no debug renderer. Defined in PhysicsDebugDraw.cpp, so the core TU does not pull Jolt's
        // debug renderer and DebugWirePass into whatever links it.
        DebugDrawStats DrawDebug(DebugWirePass& wire, const DirectX::XMFLOAT3& cameraPos);
#endif

    private:
        struct Impl;

#ifdef JPH_DEBUG_RENDERER
        // What DrawDebug needs from the pImpl: the Jolt world, the character controllers and the slot
        // holding the debug renderer (a shared_ptr, so its deleter is made in PhysicsDebugDraw.cpp).
        struct DebugTarget
        {
            JPH::PhysicsSystem*                       physics  = nullptr;
            std::vector<const JPH::CharacterVirtual*> characters;
            std::shared_ptr<PhysicsDebugDraw>*        renderer = nullptr;
        };

        DebugTarget GetDebugTarget();
#endif

        // Substep index within the current frame: PreAllSteps zeroes it, StepOnce increments it. The
        // LT-DESYNC detector runs only on the first substep: PostAllSteps writes LocalTransform once per
        // frame, so on later substeps it is stale by construction.
        std::uint32_t m_stepIndexInFrame = 0;

        // The substeps the current frame will run (SetFrameSubsteps).
        unsigned m_frameSubsteps = 1;

        std::unique_ptr<Impl> m_impl;
    };
}
