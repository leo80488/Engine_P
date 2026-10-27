// Shared particle GPU data layouts. They must match include/Graphics/ParticleSystem.h
// (ParticleGPU / ParticleEmitterGPU / ParticleSystemCB).

#ifndef PARTICLE_HLSLI
#define PARTICLE_HLSLI

// 128 bytes: ParticleGPU on the CPU and the StructureByteStride of the pool's SRV
// and UAV. Do not drop the trailing _pad5: without it the struct is 124 bytes (a
// structured-buffer element gets no tail rounding; the C++ side gets 4 bytes from
// alignas(16)), the shaders step through the pool 124 bytes at a time while the
// views say 128, and the VS reads whole buckets as dead. The [LAYOUT] check fails
// the run if the stride and sizeof drift apart (spec "Particle",
// src/Renderer/LayoutSpecs.cpp).
struct Particle
{
    float3 position;            // 12
    float  lifetime;            // 4: seconds remaining; <= 0 = dead
    float3 velocity;            // 12
    float  maxLifetime;         // 4: total life (for the 0..1 lerp)
    float4 startColor;          // 16
    float4 endColor;            // 16
    float  startSize;           // 4
    uint   textureBindlessIdx;  // 4: 0xFFFFFFFFu = no texture
    uint   visualMode;          // 4: ParticleVisualMode enum cast
    uint   materialIndex;       // 4: custom-shader bucket / material slot (0 = default)
    float  phaseSeed;           // 4: per-particle random [0,1) for the VS animation phase
    uint   orientation;         // 4: ParticleOrientation enum (billboard / horizontal / vertical / velocity)
    float  intensity;           // 4: emissive brightness multiplier (HDR)
    float  stretch;             // 4: velocity-stretch length factor
    float3 startScale;          // 12: per-axis size multiplier (offset 96, no float3 straddle)
    float  drag;                // 4: velocity damping /s (used by the update CS)
    float  emissiveFade;        // 4: intensity fade-over-life exponent (0 = constant)
    uint   flags;               // 4: PARTICLE_FLAG_* bitmask
    float  fade;                // 4: AMBIENT edge x approach fade [0,1]; written by
                                //     ParticleUpdate for AMBIENT particles, read by particle_vertex.
                                //     Non-ambient particles: unused (VS gates on the AMBIENT flag).
    float  _pad5;               // 4: pads the element to 128 (see above)
};

// Per-particle flag bits (mirror kParticleFlag* in ParticleSystem.h).
#define PARTICLE_FLAG_FLOCK 1u   // Boids agent: owned by ParticleBoidSteer, skipped by ParticleUpdate
// AMBIENT camera-follow toroidal-wrap particle: ParticleUpdate wraps position into a
// camera-anchored box, computes `fade`, and re-arms lifetime (immortal). Volume slot 0..3
// lives in flags bits 2-3 so the Update CS can index the per-volume params in ParticleSystemCB.
#define PARTICLE_FLAG_AMBIENT     2u
#define PARTICLE_AMBIENT_VOL_SHIFT 2u
#define PARTICLE_AMBIENT_VOL_MASK  3u

// 272 bytes. Matches ParticleEmitterGPU.
struct ParticleEmitter
{
    float3   position;
    float    startLifetime;
    float3   velocityMin;
    float    startSize;
    float3   velocityMax;
    uint     spawnCount;
    float4   startColor;
    float4   endColor;
    float3   gravity;
    uint     writeCursor;
    uint     randomSeed;
    uint     shapeType;          // ParticleShape enum
    uint     visualMode;         // ParticleVisualMode enum
    uint     textureBindlessIdx; // 0xFFFFFFFFu = none
    float4   shapeParam0;
    float4   shapeParam1;
    float3   startScale;         // per-axis size multiplier
    float    intensity;          // emissive brightness multiplier
    float    velocityStretch;    // streak length factor
    float    sizeVariance;       // per-particle size jitter 0..1
    float    lifeVariance;       // per-particle lifetime jitter 0..1
    float    drag;               // velocity damping /s
    float4x4 meshWorldMatrix;
    uint     meshDescSlot;
    uint     meshIndexCount;
    uint     orientation;        // ParticleOrientation enum
    float    emissiveFade;       // intensity fade-over-life exponent
    uint     maxLive;            // ring window (spawns wrap within this many slots)
    uint     ambientFlags;       // bit0 = ambient emitter, bits1-2 = volume slot 0..3
    uint     _padE1;
    uint     _padE2;
};

// Shape type constants (must match C++ ParticleShape enum).
#define PARTICLE_SHAPE_POINT   0u
#define PARTICLE_SHAPE_SPHERE  1u
#define PARTICLE_SHAPE_CONE    2u
#define PARTICLE_SHAPE_BOX     3u
#define PARTICLE_SHAPE_CIRCLE  4u
#define PARTICLE_SHAPE_MESH    5u
// Weapon-swing sparks. Carries the chord the blade tip swept this frame plus the
// tip's velocity in the otherwise idle shape params (no struct growth, no root-sig
// change):
//   emitter.position        = current tip (chord end)
//   emitter.shapeParam0.xyz = previous tip (chord start)
//   emitter.shapeParam1.xyz = tip velocity (m/s), .w = inherit weight
#define PARTICLE_SHAPE_TRAIL   6u

// Visual mode constants (must match C++ ParticleVisualMode enum).
#define PARTICLE_VISUAL_FLAT     0u
#define PARTICLE_VISUAL_FIRE     1u
#define PARTICLE_VISUAL_SMOKE    2u
#define PARTICLE_VISUAL_ELECTRIC 3u

// Orientation constants (must match C++ ParticleOrientation enum).
#define PARTICLE_ORIENT_BILLBOARD  0u
#define PARTICLE_ORIENT_HORIZONTAL 1u
#define PARTICLE_ORIENT_VERTICAL   2u
#define PARTICLE_ORIENT_VELOCITY   3u

// The emitter's `orientation` field (low byte = ParticleOrientation enum) carries
// the flock flag in bit 8, so ParticleEmit can bake PARTICLE_FLAG_FLOCK into each
// spawned particle without an extra field. CollectEmitters sets it; the emit
// shader masks the orientation back to 0xFF.
#define PARTICLE_ORIENT_MASK       0xFFu
#define PARTICLE_ORIENT_FLOCK_BIT  0x100u

// Bucketing: the pool is split into this many equal contiguous slices. Must match
// ParticleSystem::kMaxParticleBuckets (C++). The emit shader wraps each spawn batch
// within its slice = pool capacity / PARTICLE_BUCKET_COUNT.
#define PARTICLE_BUCKET_COUNT 8u

// Camera-following ambient wrap volumes. Must match kMaxAmbientVolumes in
// ParticleSystem.h (also bounded by PARTICLE_AMBIENT_VOL_MASK + 1).
#define PARTICLE_MAX_AMBIENT_VOLUMES 2u

// 304 bytes. Matches ParticleSystemCB (a root CBV). Carries per-volume ambient
// params indexed by the particle's volume slot (flags bits 2-3); rows are
// float3 + scalar so they pack into 16 B registers.
struct ParticleSystemParams
{
    float  deltaTime;
    uint   particleCount;
    uint   frameIndex;
    float  timeWrapped;          // reserved: always 0 (meant for fmod(engineTime, period))
    float3 globalGravity;
    uint   ambientActiveMask;    // bit i = ambient volume slot i active this frame
    float3 cameraPos;            // this frame's resolved camera (for the approach fade)
    float  _padC;
    // Per-volume rows (slot-indexed):
    float4 ambientVolA[PARTICLE_MAX_AMBIENT_VOLUMES]; // xyz = halfExtents H, w = edgeFade band (m)
    float4 ambientVolB[PARTICLE_MAX_AMBIENT_VOLUMES]; // xyz = center (world), w = population count
    float4 ambientVolC[PARTICLE_MAX_AMBIENT_VOLUMES]; // xyz = wind (m/s), w = nearFade0 (m)
    float4 ambientVolD[PARTICLE_MAX_AMBIENT_VOLUMES]; // x = nearFade1, y = expectedBucket (float VALUE), z = intensity ramp, w = textureBindlessIdx (float VALUE, -1 = none)
    // Live appearance rows, re-applied to every ambient particle each frame so
    // sibling ParticleRenderComponent edits stay live (not frozen at fill time).
    float4 ambientVolE[PARTICLE_MAX_AMBIENT_VOLUMES]; // startColor rgba
    float4 ambientVolF[PARTICLE_MAX_AMBIENT_VOLUMES]; // endColor rgba
    float4 ambientVolG[PARTICLE_MAX_AMBIENT_VOLUMES]; // xyz = startScale, w = startSize
    float4 ambientVolH[PARTICLE_MAX_AMBIENT_VOLUMES]; // x = intensity, y = emissiveFade, z = velocityStretch, w = packed(visualMode + orientation*4) float VALUE
};

// 32 bytes. Stable per-frame snapshot of a particle's flocking state, written by
// ParticleBoidSnapshot.cs.hlsl and read by ParticleBoidSteer.cs.hlsl. Reading this
// instead of the pool being written removes the neighbour read-while-write race.
// Matches kBoidSnapStride in ParticleSystem.h.
struct BoidSnap
{
    float3 pos;     // 12
    uint   valid;   // 4: 1 if a live FLOCK agent, else 0 (skipped by the neighbour scan)
    float3 vel;     // 12
    float  _pad;    // 4
};

// Boids uniform spatial grid (mirror kBoidGrid* in ParticleSystem.h).
#define BOID_GRID_DIM      16u
#define BOID_GRID_CELLS    (BOID_GRID_DIM * BOID_GRID_DIM * BOID_GRID_DIM)  // 4096
#define BOID_MAX_PER_CELL  64u

// 144 bytes. Matches BoidParamsCB in ParticleSystem.h.
struct BoidParams
{
    float  deltaTime;
    uint   particleCount;
    uint   sliceLen;
    float  perceptionRadius;
    float  separationRadius;
    float  separationWeight;
    float  alignmentWeight;
    float  cohesionWeight;
    float  maxSpeed;
    float  maxForce;
    float  boundsWeight;
    float  boundsRadius;
    float3 boundsCenter;
    float  _pad0;
    float4 startColor;
    float3 startScale;
    float  startSize;
    float  intensity;
    uint   visualMode;
    uint   orientation;
    uint   textureBindlessIdx;
    float3 gridOrigin;
    float  cellSize;
    uint   flockCount;
    uint   dispatchBase;   // first pool slot the boid dispatches cover
    uint   _pad2;
    uint   _pad3;
};

// Position -> clamped integer cell coord -> linear cell index.
int3 BoidCellCoord(float3 pos, float3 origin, float cellSize)
{
    int3 c = (int3)floor((pos - origin) / max(cellSize, 1e-4));
    return clamp(c, int3(0, 0, 0), int3(BOID_GRID_DIM - 1, BOID_GRID_DIM - 1, BOID_GRID_DIM - 1));
}
uint BoidCellIndex(int3 c)
{
    return (uint)(c.x + c.y * (int)BOID_GRID_DIM + c.z * (int)(BOID_GRID_DIM * BOID_GRID_DIM));
}

// Cheap integer hash -> unit float. PCG-based; deterministic.
float RandFromSeed(inout uint seed)
{
    seed = seed * 747796405u + 2891336453u;
    uint word = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word & 0x00FFFFFFu) / float(0x01000000u);
}

float3 RandVec3(inout uint seed)
{
    return float3(RandFromSeed(seed), RandFromSeed(seed), RandFromSeed(seed));
}

#endif // PARTICLE_HLSLI
