// Shared GPU layouts of the tracer system (cylindrical-billboard thin lasers). They must match
// include/Graphics/TracerSystem.h.

#ifndef TRACER_HLSLI
#define TRACER_HLSLI

// 64 bytes. Mirrors C++ TracerGPU.
struct Tracer
{
    float3 startPos;
    float  lifetime;            // seconds remaining; <= 0 = dead
    float3 endPos;
    float  maxLifetime;
    float4 color;               // linear HDR; .a = intensity multiplier
    float  width;               // beam radius in world units
    float  spawnTime;           // absolute seconds, for noise-scroll continuity
    uint   noiseTexBindlessIdx; // 0xFFFFFFFFu = procedural noise
    uint   _pad0;
};

// 64 bytes: one per spawn, pushed by the CPU into the per-frame upload buffer. Same shape as Tracer
// so the CS copies it directly.
struct TracerSpawn
{
    float3 startPos;
    float  lifetime;
    float3 endPos;
    float  maxLifetime;
    float4 color;
    float  width;
    float  spawnTime;
    uint   noiseTexBindlessIdx;
    uint   _pad0;
};

// 32 bytes. Mirrors C++ TracerSystemParams. Bound at b0 space2 for the emit and update CS and at
// b2 space0 for the render pass (RenderCB).
struct TracerSystemParams
{
    float deltaTime;
    uint  poolCapacity;
    uint  spawnCount;    // valid entries in gSpawns this frame
    float time;          // global time (s); scrolls the noise in the render PS
    uint  writeCursor;   // emit CS writes to (writeCursor + threadId) % capacity
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

#endif // TRACER_HLSLI
