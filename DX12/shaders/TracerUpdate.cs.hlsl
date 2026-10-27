// Tracer update: ages every live tracer in the pool. Tracers are static beams (start/end fixed at
// spawn), so only the lifetime ticks down; the render VS emits a degenerate quad once it reaches 0.
// Dispatch: ceil(poolCapacity / 64), 1, 1. Root signature (compute):
//   b0 space2 - TracerSystemParams (deltaTime, poolCapacity)
//   u0 space2 - RWStructuredBuffer<Tracer> gPool

#include "Tracer.hlsli"

cbuffer SystemCB : register(b0, space2)
{
    TracerSystemParams gParams;
};

RWStructuredBuffer<Tracer> gPool : register(u0, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint idx = id.x;
    if (idx >= gParams.poolCapacity) return;

    Tracer t = gPool[idx];
    if (t.lifetime <= 0.0) return;

    t.lifetime -= gParams.deltaTime;
    if (t.lifetime < 0.0) t.lifetime = 0.0;
    gPool[idx] = t;
}
