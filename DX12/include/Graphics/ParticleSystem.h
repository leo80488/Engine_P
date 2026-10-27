#pragma once

// ParticleSystem - GPU particle pool and per-emitter upload records.
//
// Owns one pool of kMaxGlobalParticles slots split into kMaxParticleBuckets fixed
// slices (one per shader / blend tuple), a per-frame upload buffer of emitter
// records and a persistent ring cursor per bucket. A wrapping cursor overwrites the
// oldest particles of its window; visible pops mean the window (Max Particles) is
// too small for the spawn rate.
//
// Per frame: Renderer::BeginFrame -> CollectEmitters (CPU: spawn counts, emitter
// records, cursors); ParticleSimPass (emit CS per emitter, boid passes, update CS
// over the simulated slots); ParticleRenderPass (one instanced quad draw per used
// bucket over its active window, the VS discards dead slots).

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/MaterialCBVRing.h"   // per-bucket b8 params (material-driven)
#include "Resource/SystemHandles.h"     // Resource::TextureHandle
#include "ECS/ECS.h"

#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <DirectXMath.h>

class IGraphicsDevice;
class World;
class ShaderLibrary;
namespace ShaderReflect { struct Reflection; }   // bucket-cached pointers only
struct ParticleRenderComponent;
struct ParticleEmitterComponent;
struct ParticleFlockComponent;
struct ParticleAmbientComponent;
struct MaterialComponent;
namespace Resource {
    class TextureSystem;
    class ResourceManager;
}

// GPU particle record, 128 bytes: mirrors Particle in Particle.hlsli, and sizeof is
// the StructureByteStride of the pool's SRV/UAV. Keep _pad5 on both sides: alignas
// pads only the C++ struct, and a 124-byte HLSL mirror misreads the 128-byte stride
// (whole buckets stop drawing). The [LAYOUT] spec "Particle"
// (src/Renderer/LayoutSpecs.cpp) checks the stride.
struct alignas(16) ParticleGPU
{
    DirectX::XMFLOAT3 position;          // 12
    float             lifetime;          // 4   - seconds remaining; <= 0 = dead
    DirectX::XMFLOAT3 velocity;          // 12
    float             maxLifetime;       // 4   - total life (for the 0..1 lerp)
    DirectX::XMFLOAT4 startColor;        // 16
    DirectX::XMFLOAT4 endColor;          // 16
    float             startSize;         // 4
    uint32_t          textureBindlessIdx;// 4   - 0xFFFFFFFFu = no texture
    uint32_t          visualMode;        // 4   - ParticleVisualMode
    uint32_t          materialIndex;     // 4   - custom-shader bucket / material slot (0 = default)
    float             phaseSeed;         // 4   - per-particle random [0,1) for VS animation
    uint32_t          orientation;       // 4   - ParticleOrientation (billboard / horizontal / vertical / velocity)
    float             intensity;         // 4   - emissive brightness multiplier (HDR)
    float             stretch;           // 4   - velocity-stretch length factor (VelocityStretched mode)
    DirectX::XMFLOAT3 startScale;        // 12  - per-axis size multiplier (offset 96, no float3 straddle)
    float             drag;              // 4   - velocity damping per second (read by the update CS)
    float             emissiveFade;      // 4   - intensity fade-over-life exponent (0 = constant)
    uint32_t          flags;             // 4   - ParticleFlag: bit0 FLOCK, bit1 AMBIENT, bits2-3 ambient volume slot
    float             fade;              // 4   - AMBIENT edge x approach fade [0,1] (ParticleUpdate -> particle_vertex)
    float             _pad5;             // 4   - pads to 128; the HLSL mirror declares it too
};                                        // 128 bytes
static_assert(sizeof(ParticleGPU) == 128, "ParticleGPU layout drift");

// Per-particle flag bits (mirror PARTICLE_FLAG_* in Particle.hlsli).
//   FLOCK: a Boids agent, owned and kept immortal by the steering pass
//   (ParticleBoidSteer.cs.hlsl) and skipped by the default integrator
//   (ParticleUpdate.cs.hlsl).
//   AMBIENT: a camera-follow toroidal-wrap particle. ParticleUpdate wraps it into a
//   camera-anchored box, fades it near the faces and keeps it immortal. Bits 2-3
//   hold its volume slot, which indexes the per-volume rows of ParticleSystemCB.
static constexpr uint32_t kParticleFlagFlock       = 1u;
static constexpr uint32_t kParticleFlagAmbient     = 2u;
static constexpr uint32_t kParticleAmbientVolShift = 2u;
static constexpr uint32_t kParticleAmbientVolMask  = 3u;

// Per-frame per-emitter upload record, consumed by ParticleEmit.cs.hlsl. 272 bytes;
// the emit dispatch binds it at a 256B-aligned kEmitterSlotStride (512 B) offset.
struct alignas(16) ParticleEmitterGPU
{
    DirectX::XMFLOAT3   position;         // 12 - world position
    float               startLifetime;    // 4
    DirectX::XMFLOAT3   velocityMin;      // 12
    float               startSize;        // 4
    DirectX::XMFLOAT3   velocityMax;      // 12
    uint32_t            spawnCount;       // 4
    DirectX::XMFLOAT4   startColor;       // 16
    DirectX::XMFLOAT4   endColor;         // 16
    DirectX::XMFLOAT3   gravity;          // 12
    uint32_t            writeCursor;      // 4
    uint32_t            randomSeed;       // 4
    uint32_t            shapeType;        // 4 - ParticleShape
    uint32_t            visualMode;       // 4 - ParticleVisualMode
    uint32_t            textureBindlessIdx;// 4 - 0xFFFFFFFFu = none

    // Shape parameters, by shapeType:
    //   Point:   unused
    //   Sphere:  p0 = (radius, spawnOnShell, _, _)
    //   Cone:    p0 = (0, halfAngle, _, _); p1 = (dir.xyz, length)
    //   Box:     p0 = (hx, hy, hz, _)
    //   Circle:  p0 = (radius, _, _, _); p1 = (normal.xyz, _)
    //   Mesh:    unused (meshWorldMatrix + meshDescSlot)
    //   Trail:   p0 = (prevTip.xyz, _); p1 = (tipVelocity.xyz, inheritWeight): the chord
    //            the blade tip swept this frame, whose END (the current tip) is `position`.
    DirectX::XMFLOAT4   shapeParam0;      // 16
    DirectX::XMFLOAT4   shapeParam1;      // 16
    // Shape-independent per-particle look, copied into each spawned particle by
    // ParticleEmit.cs.hlsl.
    DirectX::XMFLOAT3   startScale;       // 12 - per-axis size multiplier
    float               intensity;        // 4  - emissive brightness multiplier
    // Spark controls: velocityStretch, drag and emissiveFade are baked into each
    // particle; sizeVariance / lifeVariance are consumed at emit time.
    float               velocityStretch;  // 4 - streak length factor
    float               sizeVariance;     // 4 - per-particle size jitter 0..1
    float               lifeVariance;     // 4 - per-particle lifetime jitter 0..1
    float               drag;             // 4 - velocity damping per second

    DirectX::XMFLOAT4X4 meshWorldMatrix;  // 64 - Mesh shape only

    uint32_t            meshDescSlot;     // 4 - index into the bindless MeshDescriptor buffer
    uint32_t            meshIndexCount;   // 4
    uint32_t            orientation;      // 4 - ParticleOrientation (| 0x100 = FLOCK), copied to each particle
    float               emissiveFade;     // 4 - intensity fade-over-life exponent

    uint32_t            maxLive;          // 4 - ring window (<= kParticlesPerBucket): spawns wrap within it
    uint32_t            ambientFlags;     // 4 - bit0 = ambient emitter, bits1-2 = volume slot 0..3
    uint32_t            _padE1;           // 4
    uint32_t            _padE2;           // 4
};                                         // 272 bytes total
static_assert(sizeof(ParticleEmitterGPU) == 272, "Emitter CB layout drift (must match Particle.hlsli)");

// Max simultaneous camera-following ambient wrap volumes; they occupy buckets
// [kAmbientBucketBase, kAmbientBucketBase + kMaxAmbientVolumes). 2 is a budget, not a
// limit: each further volume costs 128 B of ParticleSystemCB (rows A-H) and, while
// enabled, one fixed bucket ordinary emitters cannot claim; the 2-bit slot field of
// the particle flags (bits 2-3) holds at most 4. MUST match
// PARTICLE_MAX_AMBIENT_VOLUMES in Particle.hlsli.
static constexpr uint32_t kMaxAmbientVolumes = 2;

// Per-frame global state CB of the update / render passes (a root CBV, 304 B with
// the ambient wrap-volume table). Rows are float3 + scalar so each packs into one
// 16B HLSL register (no straddles).
struct alignas(16) ParticleSystemCB
{
    float             deltaTime;         // 0
    uint32_t          particleCount;     // 4 - total pool capacity
    uint32_t          frameIndex;        // 8
    float             timeWrapped;       // 12 - reserved, written 0
    DirectX::XMFLOAT3 globalGravity;     // 16 - gravity of the first spawning standard emitter
    uint32_t          ambientActiveMask; // 28 - bit i = ambient volume slot i active this frame
    DirectX::XMFLOAT3 cameraPos;         // 32 - this frame's resolved camera (approach fade)
    float             _padC;             // 44
    // Per-volume rows, indexed by the slot in the particle's flags bits 2-3:
    DirectX::XMFLOAT4 ambientVolA[kMaxAmbientVolumes]; // 48  xyz = halfExtents H, w = edgeFade band (m)
    DirectX::XMFLOAT4 ambientVolB[kMaxAmbientVolumes]; // 80  xyz = center (world), w = population count
    DirectX::XMFLOAT4 ambientVolC[kMaxAmbientVolumes]; // 112 xyz = wind (m/s), w = nearFade0 (m)
    // 144: x = nearFade1, y = expectedBucket, z = intensity ramp, w = textureBindlessIdx
    // (-1 = none); y and w are float VALUES, not bit-casts.
    DirectX::XMFLOAT4 ambientVolD[kMaxAmbientVolumes];
    // Live appearance rows (AmbientVolumeCPU::ap*): ParticleUpdate.cs re-applies them
    // to every ambient particle each frame, so inspector edits to colour, size, etc.
    // stay live after the one-shot Box fill.
    DirectX::XMFLOAT4 ambientVolE[kMaxAmbientVolumes]; // 176 startColor rgba
    DirectX::XMFLOAT4 ambientVolF[kMaxAmbientVolumes]; // 208 endColor rgba
    DirectX::XMFLOAT4 ambientVolG[kMaxAmbientVolumes]; // 240 xyz = startScale, w = startSize
    // 272: x = intensity, y = emissiveFade, z = velocityStretch,
    // w = visualMode + orientation*4 as a float VALUE.
    DirectX::XMFLOAT4 ambientVolH[kMaxAmbientVolumes];
};
static_assert(sizeof(ParticleSystemCB) == 304, "ParticleSystemCB layout drift (must match Particle.hlsli)");

// CPU-side per-volume state, refreshed every CollectEmitters from live component
// values (so inspector edits are live) and packed into ParticleSystemCB by
// UpdateSystemCB. Slot-indexed, parallel to the flags-bits-2-3 volume slot.
struct AmbientVolumeCPU
{
    DirectX::XMFLOAT3 halfExt   = { 1.0f, 1.0f, 1.0f }; // CPU-clamped >= 0.01
    float             edgeFade  = 1.0f;
    DirectX::XMFLOAT3 center    = { 0.0f, 0.0f, 0.0f };  // cameraPos + authored offset
    float             count     = 0.0f;                  // live population (float, exact <= 2^24)
    DirectX::XMFLOAT3 wind      = { 0.0f, 0.0f, 0.0f };
    float             nearFade0 = 0.05f;
    float             nearFade1 = 0.6f;
    uint32_t          bucketId  = 0;                      // expected pool-slice bucket (= kAmbientBucketBase + slot)
    float             intensity = 1.0f;                   // runtime fade ramp [0,1]
    // Live appearance mirror of the sibling ParticleRenderComponent, refreshed every
    // CollectEmitters and packed into ambientVolE..H and ambientVolD.w.
    DirectX::XMFLOAT4 apStartColor   = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 apEndColor     = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 apStartScale   = { 1.0f, 1.0f, 1.0f };
    float             apStartSize    = 0.1f;
    float             apIntensity    = 1.0f;               // render brightness (distinct from `intensity` fade ramp)
    float             apEmissiveFade = 0.0f;
    float             apStretch      = 0.0f;               // velocityStretch
    uint32_t          apVisualMode   = 0;
    uint32_t          apOrientation  = 0;
    int32_t           apTextureIdx   = -1;                 // -1 = no texture
};

// Boids uniform spatial grid: a fixed BOID_GRID_DIM^3 grid (cellSize set to the
// perception radius so a 3x3x3 neighbourhood covers it) sized around the flock
// home each frame. cellCount[cell] + cellItems[cell*BOID_MAX_PER_CELL+k] form a
// fixed-slice per-cell directory (ClusterCull style, no prefix-sum). MUST match
// the BOID_GRID_* defines in Particle.hlsli.
static constexpr uint32_t kBoidGridDim     = 16;
static constexpr uint32_t kBoidGridCells   = kBoidGridDim * kBoidGridDim * kBoidGridDim; // 4096
static constexpr uint32_t kBoidMaxPerCell  = 64;

// Per-frame Boids params CB of the grid-build and steering passes (the snapshot
// pass reads only particleCount and dispatchBase). Sampled from the FIRST enabled
// flock, like globalGravity. Matches BoidParams in Particle.hlsli.
struct alignas(16) BoidParamsCB
{
    float             deltaTime;        // 4
    uint32_t          particleCount;    // 4  - pool capacity (dispatch bound)
    uint32_t          sliceLen;         // 4  - flock slice width (= kParticlesPerBucket)
    float             perceptionRadius; // 4  - cohesion / alignment neighbour radius (= grid cell size)
    float             separationRadius; // 4  - closer radius for the push-apart rule
    float             separationWeight; // 4
    float             alignmentWeight;  // 4
    float             cohesionWeight;   // 4
    float             maxSpeed;         // 4  - velocity clamp (world units/s)
    float             maxForce;         // 4  - per-rule steering acceleration clamp
    float             boundsWeight;     // 4  - soft spherical containment strength (0 = off)
    float             boundsRadius;     // 4
    DirectX::XMFLOAT3 boundsCenter;     // 12 - flock home (emitter world position)
    float             _pad0;            // 4
    // ---- Appearance ----
    // Re-applied to every agent by the steer pass, so inspector edits reach a one-shot
    // flock without a respawn.
    DirectX::XMFLOAT4 startColor;       // 16
    DirectX::XMFLOAT3 startScale;       // 12
    float             startSize;        // 4
    float             intensity;        // 4
    uint32_t          visualMode;       // 4
    uint32_t          orientation;      // 4  - ParticleOrientation (low byte; no flock bit here)
    uint32_t          textureBindlessIdx;// 4 - 0xFFFFFFFFu = none
    // ---- Spatial grid + population ----
    DirectX::XMFLOAT3 gridOrigin;       // 12 - min corner = boundsCenter - 0.5*dim*cellSize
    float             cellSize;         // 4  - max(perceptionRadius, 0.01)
    uint32_t          flockCount;       // 4  - live agents; steer kills slice slots >= this
    uint32_t          dispatchBase;     // 4  - first pool slot the boid dispatches cover (slot = base + thread id)
    uint32_t          _pad2;            // 4
    uint32_t          _pad3;            // 4
};
static_assert(sizeof(BoidParamsCB) == 144, "BoidParamsCB layout drift");

class ParticleSystem
{
public:
    // Pool capacity across the whole world: 1M x 128 B = 128 MiB (+ 32 MiB boid
    // snapshot), allocated on first use. Ring cursors are per bucket and every write
    // index fits a uint32. A big pool costs VRAM and the update dispatch (bounded by
    // GetSimulatedSlotCount); the render pass draws only each bucket's active window
    // (ParticleBucket::drawCount), so a large slice stays cheap for an emitter with a
    // small Max Particles. That decoupling is what allows a ceiling this high.
    static constexpr uint32_t kMaxGlobalParticles = 1048576;

    // Upper bound on emitter records per frame (272 B ParticleEmitterGPU at a 512 B
    // CBV-aligned slot stride).
    static constexpr uint32_t kMaxEmittersPerFrame = 256;

    // The pool is split into this many fixed contiguous slices, one per distinct bucket
    // tuple (custom VS / PS, blend, ...), so emitters with different shaders or blends
    // draw separately. A slice caps its tuple's live particles at kParticlesPerBucket:
    // tune DOWN (fewer, bigger slices) if a dense default effect pops, UP if many
    // distinct custom-shader emitters run at once.
    static constexpr uint32_t kMaxParticleBuckets = 8;
    static constexpr uint32_t kParticlesPerBucket = kMaxGlobalParticles / kMaxParticleBuckets;
    static_assert(kMaxGlobalParticles % kMaxParticleBuckets == 0,
                  "pool must divide evenly into buckets for equal contiguous slices");

    struct PendingMeshResolve
    {
        uint32_t emitterSlotIndex;  // index into the upload buffer
        Entity   sourceEntity;      // mesh source entity
    };

    // Byte size of one emitter upload slot: a multiple of 256 (D3D12 CBV offset
    // alignment) and >= sizeof(ParticleEmitterGPU) (272 B).
    static constexpr uint64_t kEmitterSlotStride = 512;
    static_assert(kEmitterSlotStride >= sizeof(ParticleEmitterGPU)
                  && (kEmitterSlotStride % 256) == 0,
                  "emitter slot must fit the record and stay 256B CBV-aligned");

    // ---- Boids / flocking ----
    // Byte size of one boid snapshot record ({float3 pos; uint valid; float3 vel; pad}).
    // MUST match BoidSnap in Particle.hlsli.
    static constexpr uint32_t kBoidSnapStride = 32;

    // ---- Bucketed draws ----
    struct ParticleBucket
    {
        std::string customVertexPath;   // "" = engine default Particle_VS
        std::string customSurfacePath;  // "" = engine default Particle_PS
        uint32_t    blendMode  = 1;     // ParticleBlendMode
        uint32_t    taaExclude = 0;     // 1 = stamp the TAA-exclude stencil bit (sharp VFX)
        uint32_t    lit        = 0;     // 1 = forward-lit (PARTICLE_LIT) instead of emissive
        uint32_t    receiveShadow = 0;  // 1 = the lit path also samples the sun shadow
                                        //     (PARTICLE_SHADOW). Requires lit=1; ignored
                                        //     otherwise. Deliberately NOT implied by lit:
                                        //     smoke/dust/steam want it, fire/sparks/magic
                                        //     look wrong with a shadow edge crossing them.
        uint32_t    flocking   = 0;     // 1 = Boids flock: an isolated slice, so the persistent
                                        //     agent set isn't ring-overwritten by other emitters
        uint32_t    ambient    = 0;     // 0 = ordinary; 1 + volumeSlot for a camera-follow AMBIENT
                                        //     volume. Ambient buckets live at fixed IDs
                                        //     kAmbientBucketBase + slot so an immortal wrap field's
                                        //     pool slice stays stable across frames (bucket IDs are
                                        //     otherwise re-discovered every frame).
        uint32_t    bucketBase = 0;     // = bucketId * kParticlesPerBucket (first pool slot)
        uint32_t    liveCount  = 0;     // particles spawned into the slice this frame (info/log)
        uint32_t    window     = 0;     // shared ring width (= Max Particles) for this bucket's
                                        //   cooperative cursor. Set once per frame by the FIRST
                                        //   emitter that spawns into the bucket (first-emitter-wins,
                                        //   like gravity/flock params) so every co-bucketed emitter
                                        //   advances the shared cursor by the SAME modulus. 0 = unset
                                        //   this frame. Reset each frame in CollectEmitters.
        uint32_t    drawCount  = 0;     // # instances the render pass draws for this bucket = the
                                        //   active window. Particles only ever live in
                                        //   [bucketBase, bucketBase+window), so drawing this instead
                                        //   of the full slice makes a large slice capacity FREE for
                                        //   emitters that use few particles. PERSISTS across frames
                                        //   (NOT reset in the per-frame loop) so a bucket that has
                                        //   lingering particles but doesn't spawn this frame keeps
                                        //   drawing them; refreshed to `window` on each spawn.
        bool        used       = false; // claimed this frame (also by a draining, disabled emitter)

        // ---- Material-driven b8 params ----
        // When the FIRST emitter claiming this bucket has a MaterialComponent custom
        // shader, its b8 cbuffer is packed each frame from the material's customParams /
        // customTextures (first-emitter-wins, like window). paramCbvVA 0 = nothing to bind
        // (no material custom shader, or the shader declares no b8). paramReady = false:
        // packing failed (shader still compiling, ring full), and the render pass SKIPS the
        // bucket rather than draw with an unbound b8.
        uint64_t    paramCbvVA  = 0;
        bool        paramReady  = true;
        bool        paramPacked = false;   // per-frame "already packed" latch

        // ---- Dynamic-shader resolve cache ----
        // Ids resolve once at bucket claim (a path change resets them), which keeps
        // RegisterDynamic's string key and global mutex off the per-frame path; reflection
        // pointers are re-fetched when the library's reload epoch changes. ~0u mirrors
        // ShaderLibrary::kInvalidDynShaderID.
        uint32_t    dynVsId   = ~0u;
        uint32_t    dynPsId   = ~0u;
        const ShaderReflect::Reflection* reflVS = nullptr;
        const ShaderReflect::Reflection* reflPS = nullptr;
        uint32_t    reflEpoch = 0;         // 0 = never fetched
    };

    // First bucket ID reserved for ambient wrap volumes: volume slot v uses the FIXED
    // bucket kAmbientBucketBase + v, so an immortal wrap field's slice never moves while
    // ordinary bucket IDs are re-discovered every frame (a moving slice would orphan or
    // overwrite the population). Ordinary emitters take any bucket not reserved this
    // frame, so with no active ambient volume they get the full [0, kMaxParticleBuckets)
    // range. Bucket 0 is never reserved: it is the overflow fallback.
    static constexpr uint32_t kAmbientBucketBase = 1;
    static_assert(kMaxAmbientVolumes < kMaxParticleBuckets,
                  "need at least one ordinary bucket left over");

    // ---- Lifetime, emitter collection and GPU buffers ----
    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);

    // Wires the systems that resolve emitter texture paths to bindless indices.
    // Optional: unset, emitter textures are ignored.
    void SetResourceSystems(Resource::TextureSystem* texSys,
                            Resource::ResourceManager* resMgr)
    {
        m_texSys = texSys;
        m_resMgr = resMgr;
    }

    // Custom shaders register/reflect against the RENDER pass's library so
    // param reflection and the PSO cache agree on dynamic shader IDs (same
    // pattern as TrailSystem). Wired by Renderer after pass creation.
    void SetShaderLibrary(ShaderLibrary* lib) { m_shaderLib = lib; }

    // Releases the entity's cached emitter / custom-shader textures and frees its ambient
    // volume slot. Fan-in via Renderer's entity-destroy listener.
    void OnEntityDestroyed(IGraphicsDevice& gfx, Entity e);

    // Called from Renderer::OnWorldClear. World::Clear() fires no per-entity destroy
    // listeners, so this drops the ambient volume slots and releases the cached emitter
    // textures (TextureSystem::Release defers the GPU destroy, so this is safe
    // mid-frame), and unless @p worldSurvives it releases the particle pools.
    // worldSurvives = true is the bake-preview clear where the World and its emitters
    // keep rendering: do not release the pools there, the spawn latches would still
    // read "already spawned" against a zeroed pool.
    void OnWorldClear(IGraphicsDevice& gfx, bool worldSurvives = false);

    // Called each frame from Renderer::BeginFrame, before any pass records: allocates
    // the pools on first use, advances each emitter's spawn accumulator, writes the
    // ParticleEmitterGPU records into this frame's upload slot, claims buckets,
    // advances the ring cursors, and queues mesh-shape emitters for Renderer to patch
    // (meshDescSlot + meshWorldMatrix) right after.
    void CollectEmitters(World& world, float dt, uint32_t frameIndex,
                         const DirectX::XMFLOAT3& cameraPos);

    // Pending mesh reference of a mesh-shape emitter: filled by CollectEmitters,
    // consumed by Renderer::BeginFrame right after.
    const std::vector<PendingMeshResolve>& GetPendingMeshResolves() const
    { return m_pendingMeshResolves; }

    // Patches the upload slot at `slotIndex` with mesh descriptor slot +
    // world matrix. Called by Renderer after CollectEmitters.
    void PatchMeshEmitter(uint32_t slotIndex,
                          uint32_t meshDescSlot,
                          uint32_t meshIndexCount,
                          const DirectX::XMFLOAT4X4& worldMatrix);

    // Number of emitter slots written this frame (for dispatch count).
    uint32_t GetEmitterCount()  const { return m_emitterCountThisFrame; }

    // Spawn count for emitter slot `i` (0..GetEmitterCount-1). Used by emit
    // dispatch to size the threadgroup count (ceil(spawnCount/64)).
    uint32_t GetEmitterSpawnCount(uint32_t i) const
    { return i < m_emitterSpawnCounts.size() ? m_emitterSpawnCounts[i] : 0; }

    // Descriptor accessors used by passes.
    const RHI::GPUBuffer& GetParticlePool()    const { return m_particlePool; }
    // One emitter buffer per frame in flight; returns the current frame's.
    const RHI::GPUBuffer& GetEmitterBuffer()   const;
    uint64_t              GetPoolSRVHandle()   const { return m_poolSrvHandle; }
    uint64_t              GetPoolUAVHandle()   const { return m_poolUavHandle; }

    // Per-frame globals CB (delta time, frame index, gravity, ambient volumes).
    const RHI::GPUBuffer& GetSystemCB()        const;
    void                  UpdateSystemCB(float dt, uint32_t frameIndex);

    // ---- Boids / flocking ----
    // A flock is a one-shot persistent population of FLOCK agents in its own bucket
    // slice. Each frame the snapshot pass copies a stable {pos, vel, valid} view into
    // m_boidSnapshot; the steer pass reads neighbours from it (no read-while-write
    // race) and rewrites the pool. Both run only while IsFlockActive(), so flock-free
    // scenes pay nothing.
    bool                  IsFlockActive()           const { return m_flockActive; }
    // Upper bound (exclusive) on pool slots that can hold a FLOCK agent: the
    // monotonic high-water of (flock bucketBase + flockCount). Agents only ever
    // spawn into [bucketBase, bucketBase + flockCount), so this bounds the
    // snapshot / grid-build / steer dispatches to the flock slices instead of the
    // whole pool. Monotonic for the same reason as m_simHighWater.
    uint32_t              GetBoidSimulatedSlotCount() const { return m_boidSimHighWater; }
    // Lower bound (inclusive) of the same range: the monotonic low-water of the flock
    // bucket bases. The kernels add BoidParams::dispatchBase (kept equal to this) to
    // the thread id, so a flock in bucket n does not pay for the buckets below it.
    uint32_t              GetBoidSimulatedSlotBase()  const { return m_boidSimLowWater; }
    const RHI::GPUBuffer& GetBoidParamsCB()         const;
    const RHI::GPUBuffer& GetBoidSnapshot()         const { return m_boidSnapshot; }
    uint64_t              GetBoidSnapshotSRVHandle() const { return m_boidSnapshotSrv; }
    uint64_t              GetBoidSnapshotUAVHandle() const { return m_boidSnapshotUav; }
    // Spatial-grid buffers (cell directory). cellCount is cleared+rebuilt each
    // frame; cellItems holds up to kBoidMaxPerCell agent indices per cell.
    const RHI::GPUBuffer& GetBoidCellCount()        const { return m_boidCellCount; }
    const RHI::GPUBuffer& GetBoidCellItems()        const { return m_boidCellItems; }
    uint64_t              GetBoidCellCountUAVHandle() const { return m_boidCellCountUav; }
    uint64_t              GetBoidCellItemsUAVHandle() const { return m_boidCellItemsUav; }
    static constexpr uint32_t GetBoidGridCells() { return kBoidGridCells; }

    // ---- Pool capacity and the update bound ----
    // Total pool slot count (convenience).
    static constexpr uint32_t GetPoolCapacity() { return kMaxGlobalParticles; }

    // Upper bound (exclusive) on pool slots that can hold a particle: the
    // monotonic high-water of (bucketBase + drawCount) across every bucket. The
    // emit CS wraps every spawn inside [bucketBase, bucketBase + maxLive) with
    // maxLive == the bucket's window, and drawCount is refreshed to that window on
    // each spawn, so nothing can ever live above this. Used to bound the per-frame
    // update dispatch to real work instead of the whole kMaxGlobalParticles pool.
    uint32_t GetSimulatedSlotCount() const { return m_simHighWater; }

    // ---- Bucketed draws ----
    // The pool is carved into kMaxParticleBuckets fixed contiguous slices, each drawn
    // with its own PSO. A bucket is a unique (custom VS, custom PS, blend, taaExclude,
    // lit, receiveShadow, flocking) tuple discovered in CollectEmitters; emitters
    // sharing a tuple share a slice. The render pass issues one DrawInstanced per used
    // bucket over its drawCount window, rebasing the instance in the VS through the b0
    // bucketBase root constant.
    const ParticleBucket* GetBuckets()    const { return m_buckets; }
    uint32_t              GetBucketCount() const { return m_bucketCount; }
    static constexpr uint32_t GetParticlesPerBucket() { return kParticlesPerBucket; }

private:
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    struct EmitterTexCache
    {
        std::string             path;
        Resource::TextureHandle handle = Resource::kInvalidTextureHandle;
    };

    // Returns the bucket for an ORDINARY emitter's (vs, ps, blend, taaExclude, lit,
    // flocking, recvShadow) tuple, claiming a free one if it is new. Skips buckets
    // reserved by active ambient volumes this frame (m_ambientReservedBucketMask); with
    // no free bucket it folds into bucket 0. @p recvShadow is part of the reuse key: it
    // selects the PARTICLE_SHADOW PSO permutation, so emitters differing only in it must
    // not share a bucket.
    uint32_t FindOrCreateBucket(const std::string& vs, const std::string& ps,
                                uint32_t blend, uint32_t taaExclude, uint32_t lit,
                                uint32_t flocking, uint32_t recvShadow);

    // Population count of an ambient volume (density x volume, or the explicit count),
    // clamped to the bucket slice.
    uint32_t ComputeAmbientCount(const ParticleAmbientComponent& ac,
                                 const DirectX::XMFLOAT3& halfExt) const;

    // ---- Lazy pool residency ----
    // The 128 MiB main pool, the 32 MiB boid snapshot and the 1 MiB boid grid are
    // allocated by CollectEmitters on the first frame the world holds the matching
    // component (a particle-free scene pays nothing) and released with the world
    // (OnWorldClear), never on an idle timer: emitter components die before the
    // particles they spawned (the VFX lane gives the emitter a LifetimeComponent while
    // the sim keeps drawing), so a timed release pops live effects, and a frame-counted
    // timer depends on the display refresh rate.
    // A release resets every ring cursor and the dispatch bounds: the replacement buffer
    // comes back zeroed, so the coverage invariant (a superset of every slot ever
    // spawned into) restarts from empty.
    void EnsurePoolResources(bool wantPool, bool wantBoids);
    void ReleaseMainPool();
    void ReleaseBoidPools();

    IGraphicsDevice* m_gfx = nullptr;

    // Main pool (GPU DEFAULT heap, SRV + UAV).
    RHI::GPUBuffer   m_particlePool;
    uint64_t         m_poolSrvHandle = 0;
    uint64_t         m_poolUavHandle = 0;

    // Boids snapshot pool (GPU DEFAULT heap, SRV + UAV). Holds a stable per-frame
    // {pos,vel,valid} copy of the pool so the steering pass reads neighbours
    // without racing its own writes. kMaxGlobalParticles * kBoidSnapStride.
    RHI::GPUBuffer   m_boidSnapshot;
    uint64_t         m_boidSnapshotSrv = 0;
    uint64_t         m_boidSnapshotUav = 0;

    // Boids uniform-grid cell directory (UAV-only). cellCount: one uint per cell
    // (atomic counter, cleared each frame). cellItems: kBoidMaxPerCell agent
    // indices per cell (fixed slice; overflow drops).
    RHI::GPUBuffer   m_boidCellCount;
    uint64_t         m_boidCellCountUav = 0;
    RHI::GPUBuffer   m_boidCellItems;
    uint64_t         m_boidCellItemsUav = 0;

    // Per-frame Boids params CB (one per frame in flight), written by CollectEmitters.
    FrameCB<BoidParamsCB> m_boidCB;
    bool             m_flockActive  = false;  // any enabled flocking emitter this frame
    BoidParamsCB     m_boidParams{};          // sampled from the first flocking emitter

    // Emitter upload buffers (UPLOAD heap, one per frame in flight, written by
    // CollectEmitters). Each ParticleEmitterGPU sits at the start of a 512 B
    // kEmitterSlotStride slot so emit dispatches bind 256B-aligned root CBVs; the rest
    // of the slot is zeroed.
    RHI::GPUBuffer   m_emitterBuffer[kFrameCount];
    void*            m_emitterMapped[kFrameCount] = {};

    // Per-frame globals CB (UPLOAD heap, root CBV, one per frame in flight).
    FrameCB<ParticleSystemCB> m_systemCB;

    // Per-bucket state. Paths, cursors and drawCount persist across frames;
    // CollectEmitters resets used / liveCount / window / param state each frame and
    // re-discovers the tuples.
    ParticleBucket   m_buckets[kMaxParticleBuckets];
    uint32_t         m_bucketCursor[kMaxParticleBuckets] = {};  // per-bucket relative write cursor
    uint32_t         m_bucketCount = 0;                         // 1 + the highest bucket ID claimed this frame (draw loop bound)

    // Compute-dispatch bounds, recomputed every CollectEmitters but only ever RAISED:
    // a bucket whose window (or a flock whose count) shrinks still holds live particles
    // above the new bound, and they must keep being dispatched, which ages them out and
    // runs the ambient over-population kill / the boid steer kill that retires the
    // surplus. Reset only when the pool is released (its replacement comes back
    // zeroed), so coverage is always a superset of every slot ever spawned into.
    uint32_t         m_simHighWater     = 0;   // update dispatch bound (pool slots)
    uint32_t         m_boidSimHighWater = 0;   // boid snapshot/grid/steer dispatch bound
    // ...and the matching LOW-water, only ever LOWERED: the boid dispatches start here
    // instead of slot 0, so a flock in bucket n does not pay for the
    // n*kParticlesPerBucket slots below it, while [low, high) stays a superset of every
    // slot any flock spawned into (the steer shrink-kill needs that).
    // kMaxGlobalParticles = no flock seen yet (empty span).
    uint32_t         m_boidSimLowWater  = kMaxGlobalParticles;

    // Per-emitter data of this frame, parallel to the upload buffer:
    //   m_emitterSpawnCounts[i] = spawn count of emitter slot i.
    //   m_globalGravity         = gravity of the first spawning standard emitter (zero if none).
    uint32_t                         m_emitterCountThisFrame = 0;
    std::vector<uint32_t>            m_emitterSpawnCounts;
    DirectX::XMFLOAT3                m_globalGravity = { 0.0f, 0.0f, 0.0f };

    // Deferred mesh-shape resolution requests (Renderer patches after call).
    std::vector<PendingMeshResolve>  m_pendingMeshResolves;

    // Texture resolution dependencies (optional: null = no texture support).
    Resource::TextureSystem*   m_texSys = nullptr;
    Resource::ResourceManager* m_resMgr = nullptr;

    // Per-entity texture cache (particle emitters do not use the renderer's
    // shared material runtime-entry ownership).
    // Keeps the acquired TextureHandle alive until the path changes or the
    // emitter is destroyed. On path change we Release the old handle so the
    // texture system can evict the image.
    std::unordered_map<Entity, EmitterTexCache> m_emitterTexCache;

    // ---- Material-driven custom shader plumbing ----
    // Mirrors TrailSystem: the render pass's ShaderLibrary (so reflection and the PSO
    // cache agree on dynamic IDs), a private CBV ring for per-bucket b8 params (the
    // renderer's material ring rotates AFTER CollectEmitters), and entity+slot-keyed
    // caches of the "Tex_*" texture slots and their bindless indices.
    ShaderLibrary*  m_shaderLib = nullptr;
    MaterialCBVRing m_paramRing;
    std::unordered_map<Entity,
        std::unordered_map<std::string, EmitterTexCache>> m_customTexCache;
    std::unordered_map<Entity,
        std::unordered_map<std::string, int32_t>>         m_customTexIndices;

    // ---- Ambient (camera-follow toroidal wrap volumes) ----
    // Per-volume params packed into ParticleSystemCB each frame, slot-indexed
    // (0..kMaxAmbientVolumes-1). m_ambientSlotByEntity keeps a slot stable per owning
    // entity, so the wrap field's pool slice never moves.
    AmbientVolumeCPU  m_ambientVolumes[kMaxAmbientVolumes];
    uint32_t          m_ambientActiveMask = 0;          // rebuilt each CollectEmitters
    uint32_t          m_ambientReservedBucketMask = 0;  // bucket IDs reserved by active volumes this frame
    // Per-slot "active last frame". An inactive -> active transition forces a refill:
    // without it a single disabled frame leaves the spawn latch set and the field never
    // refills.
    bool              m_ambientActiveLastFrame[kMaxAmbientVolumes] = {};
    bool              m_ambientActiveThisFrame[kMaxAmbientVolumes] = {};
    DirectX::XMFLOAT3 m_cameraPos = { 0.0f, 0.0f, 0.0f };
    // Persistent entity -> volume slot. Assigned on first sight; freed when the entity
    // loses its ParticleAmbientComponent (reconciled each frame), in OnEntityDestroyed
    // and in OnWorldClear.
    std::unordered_map<Entity, uint32_t> m_ambientSlotByEntity;
};
