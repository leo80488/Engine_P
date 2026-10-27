// Tracer emit: copies this frame's CPU-queued spawn records into the GPU tracer pool.
// Dispatch: ceil(spawnCount / 64), 1, 1. Root signature (compute, the ParticleEmitCS slots):
//   b0 space2 - TracerSystemParams (spawnCount, writeCursor, capacity)
//   t0 space2 - StructuredBuffer<TracerSpawn> gSpawns (UPLOAD, this frame's queue)
//   u0 space2 - RWStructuredBuffer<Tracer>    gPool

#include "Tracer.hlsli"

cbuffer SystemCB : register(b0, space2)
{
    TracerSystemParams gParams;
};

StructuredBuffer<TracerSpawn>   gSpawns : register(t0, space2);
RWStructuredBuffer<Tracer>      gPool   : register(u0, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint spawnIdx = id.x;
    if (spawnIdx >= gParams.spawnCount) return;

    TracerSpawn s = gSpawns[spawnIdx];

    const uint dst = (gParams.writeCursor + spawnIdx) % gParams.poolCapacity;

    Tracer t;
    t.startPos            = s.startPos;
    t.lifetime            = s.lifetime;
    t.endPos              = s.endPos;
    t.maxLifetime         = s.maxLifetime;
    t.color               = s.color;
    t.width               = s.width;
    t.spawnTime           = s.spawnTime;
    t.noiseTexBindlessIdx = s.noiseTexBindlessIdx;
    t._pad0               = 0u;
    gPool[dst] = t;
}
