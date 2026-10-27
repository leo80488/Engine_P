#pragma once

// Particle components, a composition model: an entity that draws particles carries ONE
// ParticleRenderComponent (the shared look) plus exactly ONE behaviour component that decides how
// the particles spawn and move:
//   ParticleRenderComponent   - appearance: colour, size, blend, texture, custom shader,
//                               orientation, TAA / lit. Shared by every behaviour (one VS / PS /
//                               bucket path).
//   ParticleEmitterComponent  - the standard emitter: continuous / burst spawn from a shape,
//                               gravity + drag.
//   ParticleFlockComponent    - boids: a one-shot persistent flock steered on the GPU.
//   ParticleAmbientComponent  - camera-following toroidal wrap volume (dust / snow / rain /
//                               fireflies).
// The behaviours are independent (no flags, no shared fields). ParticleSystem iterates each
// behaviour pool and reads the sibling ParticleRenderComponent, using defaults when there is none.

#include "ECS/ECS.h"
#include <cstdint>
#include <string>
#include <DirectXMath.h>

enum class ParticleBlendMode : uint8_t
{
    Alpha        = 0,  // SRC_ALPHA / INV_SRC_ALPHA
    Additive     = 1,  // ONE / ONE (sparks, magic glow)
    Premultiplied= 2,  // ONE / INV_SRC_ALPHA (lit particles: add+alpha in one PSO)
    Multiply     = 3,  // DEST_COLOR / INV_SRC_ALPHA (smoke darkening)
};

// Spawn geometry. Each shape samples position + velocity differently.
// See shaders/ParticleEmit.cs.hlsl for the per-shape math.
enum class ParticleShape : uint8_t
{
    Point   = 0,  // spawn at emitter.position; velocity ~ velMin..velMax
    Sphere  = 1,  // sphere surface or volume
    Cone    = 2,  // cone with half-angle + direction
    Box     = 3,  // AABB volume
    Circle  = 4,  // disk facing `circleNormal`
    Mesh    = 5,  // random point on mesh source entity's surface triangles
    Trail   = 6,  // sparks along the chord the blade tip swept this frame, with its velocity
};

// Visual style applied in the pixel shader. Composable with texture slot.
enum class ParticleVisualMode : uint8_t
{
    Flat     = 0,  // solid color * radial alpha falloff
    Fire     = 1,  // heat ramp (black -> red -> orange -> yellow -> white)
    Smoke    = 2,  // soft desaturated, alpha from age^2
    Electric = 3,  // high-contrast emissive pulse
};

// Quad orientation. Billboard always faces the camera (sparks, glows); the fixed-plane modes keep
// a stable world orientation for sprites that must not spin with the camera (a butterfly whose
// wings flap out of a real plane, ground decals, falling leaves).
enum class ParticleOrientation : uint8_t
{
    Billboard         = 0,  // camera-facing (camRight/camUp)
    Horizontal        = 1,  // lies flat in world XZ plane, normal = +Y (seen from above)
    Vertical          = 2,  // upright in world XY plane, normal = +Z
    VelocityStretched = 3,  // camera-facing streak along velocity, longer with speed (see velocityStretch)
};

// ==== ParticleRenderComponent - shared appearance for any particle behaviour ====
// One per particle entity, beside a behaviour component. Every field feeds the GPU emitter record
// (baked into each spawned particle) or the render PSO.
struct ParticleRenderComponent
{
    DirectX::XMFLOAT4 startColor       = { 1.0f, 0.8f, 0.3f, 1.0f };
    DirectX::XMFLOAT4 endColor         = { 1.0f, 0.2f, 0.1f, 0.0f };

    float             startSize        = 0.2f;

    // Per-axis size multiplier on top of startSize: x -> quad right axis, y -> quad up axis,
    // z -> quad normal axis (no visible effect on a flat billboard; available to custom vertex
    // shaders for 3D folds / flaps). {1,1,1} = a uniform square.
    DirectX::XMFLOAT3 startScale       = { 1.0f, 1.0f, 1.0f };

    // Emissive brightness multiplier applied to the final RGB in the pixel
    // shader (after the visual-mode look). HDR: values > 1 push into bloom.
    float             intensity        = 1.0f;

    // Emissive fade over life: exponent applied to the intensity as the
    // particle ages. 0 = constant brightness. 1 = linear fade, 2 = quadratic.
    float             emissiveFade     = 0.0f;

    // Streak length factor for ParticleOrientation::VelocityStretched. The
    // quad elongates along velocity by (speed * velocityStretch) world units.
    float             velocityStretch  = 0.04f;

    ParticleBlendMode   blendMode      = ParticleBlendMode::Additive;
    ParticleVisualMode  visualMode     = ParticleVisualMode::Flat;
    ParticleOrientation orientation    = ParticleOrientation::Billboard;

    // Exclude this emitter's particles from TAA accumulation (a stencil bit TAA reads). Additive /
    // unlit VFX (sparks, electric, fire) have no velocity, so TAA ghosts and smears them: keep this on
    // (default) for crisp VFX, turn it off for large soft or lit particles (smoke, dust).
    bool                excludeFromTAA = true;

    // Forward-lit (sun + ambient IBL) instead of pure emissive. lit forces the
    // particle into full TAA (the excludeFromTAA flag is ignored while lit).
    bool                lit            = false;

    // Sample the sun's CSM + per-object shadow on the lit path. Requires `lit` (the unlit path is an
    // emissive composite with no direct term to gate).
    // A second opt-in, default off, on purpose: smoke, dust and steam should go dark under a canopy,
    // while fire, sparks and magic look wrong when a shadow edge sweeps across them. Do not derive it
    // from the material's Receive Shadow flag: that defaults to true and would switch shadows on for
    // every existing lit emitter.
    // Costs one PSO variant (PARTICLE_SHADOW) per lit bucket that asks for it and nothing for emitters
    // that do not: their PS never declares the shadow resources.
    bool                receiveShadow  = false;

    // ---- Custom shader ----
    // Opt-in per-emitter shaders: customVertexPath -> a .vs.hlsl that includes particle_vertex.hlsli
    // and defines CustomizeParticleVertex; customSurfacePath -> a .ps.hlsl that includes
    // particle_surface.hlsli and defines EvaluateParticleSurface. Empty paths use the engine defaults.
    bool              useCustomShader   = false;
    std::string       customVertexPath;     // empty = engine default VS
    std::string       customSurfacePath;    // empty = engine default PS

    // Optional texture. `textureBindlessIdx` is what the shader samples via the
    // bindless table; CPU resolves via TextureSystem on load. < 0 = no texture,
    // shader falls back to a radial alpha mask.
    int32_t           textureBindlessIdx = -1;
    std::string       texturePath;          // for serialization + editor display
    uint64_t          textureGpuHandle   = 0;  // preview handle for inspector swatch
};

// ==== ParticleEmitterComponent - the standard emitter behaviour ====
// Spawns from a shape and integrates under gravity + drag. Pair with a ParticleRenderComponent.
struct ParticleEmitterComponent
{
    bool              enabled          = true;

    // ---- Spawn rate ----
    float             spawnRate        = 50.0f;

    // Live-particle ceiling for THIS emitter (Unity-style "Max Particles").
    // Spawns wrap within a ring window of `maxParticles` slots, so steady state
    // holds at most this many. Clamped at runtime to kParticlesPerBucket.
    int               maxParticles     = 2000;

    // One-shot burst: when burstTrigger is raised, this many particles spawn in
    // a single frame (in addition to spawnRate), then the trigger clears. Use
    // spawnRate = 0 + a triggered burst for impact sparks. burstCount is
    // serialized; burstTrigger is runtime-only.
    int               burstCount       = 0;
    bool              burstTrigger     = false;   // runtime one-shot flag (not serialized)

    // ---- Per-particle initial state ----
    float             startLifetime    = 2.0f;

    // Per-particle randomisation (0 = uniform): size / lifetime *= (1 +/- var * rand).
    float             sizeVariance     = 0.0f;    // 0..1
    float             lifeVariance     = 0.0f;    // 0..1

    DirectX::XMFLOAT3 velocityMin      = { -0.5f,  1.0f, -0.5f };
    DirectX::XMFLOAT3 velocityMax      = {  0.5f,  3.0f,  0.5f };

    DirectX::XMFLOAT3 gravity          = { 0.0f, -9.8f, 0.0f };

    // Velocity damping (air drag), per second. velocity *= (1 - drag*dt).
    float             drag             = 0.0f;

    // ---- Shape ----
    ParticleShape     shape              = ParticleShape::Point;

    // Sphere params
    float             sphereRadius       = 1.0f;
    bool              sphereSpawnOnShell = true;   // true = surface, false = volume

    // Cone params
    float             coneHalfAngle      = 0.5236f;           // 30 degrees
    DirectX::XMFLOAT3 coneDirection      = { 0.0f, 1.0f, 0.0f };
    float             coneLength         = 1.0f;              // optional forward offset

    // Box params
    DirectX::XMFLOAT3 boxHalfExtents     = { 1.0f, 1.0f, 1.0f };

    // Circle params
    float             circleRadius       = 1.0f;
    DirectX::XMFLOAT3 circleNormal       = { 0.0f, 1.0f, 0.0f };

    // Mesh params: the entity whose MeshLibRef / MeshHandle provides the triangle surface, resolved
    // on the CPU each frame (MeshDescriptor + GlobalTransform).
    Entity            meshSourceEntity   = 0;  // NullEntity; 0 = unset

    // ---- Trail params (ParticleShape::Trail): weapon-swing sparks ----
    // Sparks laid along the arc a swinging blade sweeps. Put this emitter on the weapon socket (the
    // attach the TrailComponent ribbon uses) and set `bladeTipLocal` to the tip offset in the entity's
    // LOCAL space; matching the ribbon's localPoints[pointCount-1] puts the sparks on the edge. The tip
    // goes through the full world matrix (rotation included), so a blade spinning about a
    // near-stationary socket still sweeps a real arc, unlike the translation-only origin the other
    // shapes emit from.
    DirectX::XMFLOAT3 bladeTipLocal      = { 0.0f, 1.0f, 0.0f };  // matches TrailComponent's default tip

    // Fraction of the blade-tip velocity handed to each spark (0 = sparks hang
    // where they are born, 1 = they fly with the edge). Layered ON TOP of the
    // velocityMin..velocityMax random scatter, which stays the spread.
    float             inheritVelocity    = 0.7f;

    // Tip speed (m/s) below which nothing spawns, so a merely carried weapon does not spark. It is
    // absolute world speed, so it must out-run the carrier: an attached emitter inherits the
    // character's locomotion (run ~7.5 m/s), while a swing puts a 1 m blade tip at ~12-20 m/s (180 deg
    // in 0.25 s = 12.6 m/s). Keep it above run speed, or a running character trails sparks for the
    // whole VFX duration. World speed rather than carrier-relative on purpose: a lunge / dash attack
    // gets most of its blade travel from root motion, and a carrier-relative gate would emit nothing
    // during exactly those attacks.
    float             minSwingSpeed      = 8.0f;

    // Sparks per METRE of tip travel. Distance-driven (not time-driven) so a
    // fast and a slow swing lay down the same spatial density along the arc;
    // `spawnRate` is ignored for this shape.
    float             sparksPerMeter     = 40.0f;

    // ---- CPU-only per-frame state ----
    float             spawnAccumulator = 0.0f;
    // Trail shape: last frame's world tip = the swept chord's start point. Not serialized: re-seeds
    // on load / spawn (hasPrevTip false emits nothing for one frame, which stops a spawn-frame smear
    // from the origin).
    DirectX::XMFLOAT3 prevTipWorld     = { 0.0f, 0.0f, 0.0f };
    bool              hasPrevTip       = false;
    // Seconds a DISABLED emitter still claims its bucket so already-spawned
    // particles can finish dying instead of popping the frame `enabled` goes
    // false (buckets are re-claimed from scratch every frame, and an unclaimed
    // pool slice is neither simulated nor drawn). Refreshed to the worst-case
    // particle lifetime on every live frame. Not serialized: 0 means "nothing
    // of mine is in flight", which is exactly right for a freshly loaded
    // emitter that was saved disabled.
    float             drainTimer       = 0.0f;
};

// ==== ParticleFlockComponent - boids behaviour ====
// A one-shot persistent population of `flockCount` agents in its own bucket slice, kept alive and
// flocking by a GPU steering pass (separation / alignment / cohesion). No continuous spawn. Use
// ParticleOrientation::VelocityStretched on the render component for heading-aligned birds / fish.
// Re-spawns on world reload / re-add.
struct ParticleFlockComponent
{
    bool              enabled          = true;

    int               flockCount        = 300;    // agents spawned once (<= ~kParticlesPerBucket)
    float             perceptionRadius  = 4.0f;    // cohesion/alignment neighbour radius
    float             separationRadius  = 1.5f;    // closer push-apart radius
    float             separationWeight  = 1.6f;
    float             alignmentWeight   = 1.0f;
    float             cohesionWeight    = 1.0f;
    float             maxSpeed          = 6.0f;    // velocity clamp (world units/s)
    float             maxForce          = 12.0f;   // per-rule steering accel clamp
    float             boundsRadius      = 12.0f;   // soft containment sphere radius
    float             boundsWeight      = 1.2f;    // containment strength (0 = let them roam)

    // Runtime one-shot latch, set after the population spawns. Not serialized: false on load /
    // re-add, so the flock re-spawns.
    bool              flockSpawned      = false;
    int               flockSpawnedCount = -1;
};

// ==== ParticleAmbientComponent - camera-following toroidal wrap volume ====
// Pre-fills, once, a box centred on (active camera + offset) and keeps the population immortal;
// ParticleUpdate wraps particles at the faces so a fixed budget reads as an infinite field.
// World-anchored: particles hang in world space, only the sampling window follows the camera.
// Motion comes from velocityMin / Max + drag + wind. Pair with a ParticleRenderComponent.
struct ParticleAmbientComponent
{
    bool              enabled          = true;

    int               ambientCount     = 1500;               // explicit budget (used when ambientDensity <= 0)
    float             ambientDensity   = 0.0f;               // particles per m^3; > 0 overrides ambientCount (resize-invariant)
    DirectX::XMFLOAT3 ambientExtents   = { 8.0f, 4.0f, 8.0f }; // box HALF extents (m)
    DirectX::XMFLOAT3 ambientOffset    = { 0.0f, 0.0f, 0.0f }; // WORLD-axis offset from camera (box never rotates)
    float             ambientEdgeFade  = 1.5f;               // face fade band (m); 0 = hard faces (pops)
    DirectX::XMFLOAT3 ambientWind      = { 0.0f, 0.0f, 0.0f }; // constant advection (m/s)
    float             ambientNearFade0 = 0.05f;              // camera-approach fade inner radius (m)
    float             ambientNearFade1 = 0.6f;               // camera-approach fade outer radius (m)
    float             ambientIntensity = 1.0f;               // runtime fade ramp [0,1] (weather in/out); NOT serialized

    // ---- Motion (ambient integrates its own velocity + wind; no gravity) ----
    DirectX::XMFLOAT3 velocityMin      = { -0.2f, -1.0f, -0.2f };
    DirectX::XMFLOAT3 velocityMax      = {  0.2f, -0.4f,  0.2f };
    float             drag             = 0.0f;
    // 0 (default) = infinite lifetime: particles never age, so the one-shot fill does not pulse in
    // sync (a shared finite period fades the whole field together, unnatural for dust / snow). Set
    // > 0 only for a deliberate cyclic look (fireflies blinking through the start -> end colours).
    float             startLifetime    = 0.0f;

    // Runtime one-shot latches (not serialized: reset on load so the field re-fills, like
    // flockSpawned). ambientKey = hash(extents, count, density); a mismatch forces a uniform re-fill
    // after a resize / count edit.
    bool              ambientSpawned   = false;
    uint64_t          ambientKey       = 0;
};
