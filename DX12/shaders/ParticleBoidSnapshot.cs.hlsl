// Copies a stable per-frame {pos, vel, valid} view of the particle pool into the
// boid snapshot buffer. Boids steering reads every neighbour's current-frame
// position + velocity while writing next-frame state into the same pool, so
// reading the pool during the steer dispatch would race (a neighbour's thread may
// have overwritten it already); the steer pass reads neighbours from this immutable
// snapshot instead.
//
// One thread per pool slot. `valid` is 1 only for live FLOCK agents, so the
// neighbour scan ignores dead slots and non-boid particles that share the
// (isolated) bucket slice.
//
// Root signature (space2):
//   b0 -> BoidParams CBV (dispatchBase and particleCount used here)
//   u0 -> DESC_TABLE -> gPool UAV (read)
//   u1 -> DESC_TABLE -> gSnap UAV (write)

#include "Particle.hlsli"

cbuffer BoidCB : register(b0, space2)
{
    BoidParams gBoid;
};

RWStructuredBuffer<Particle> gPool : register(u0, space2);
RWStructuredBuffer<BoidSnap> gSnap : register(u1, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // Thread id is relative to the dispatched flock range, not the pool: bias it
    // back into an absolute pool slot (gPool/gSnap share pool indexing).
    const uint idx = gBoid.dispatchBase + id.x;
    if (idx >= gBoid.particleCount) return;

    Particle p = gPool[idx];

    BoidSnap s;
    s.pos   = p.position;
    s.vel   = p.velocity;
    s.valid = (((p.flags & PARTICLE_FLAG_FLOCK) != 0u) && p.lifetime > 0.0) ? 1u : 0u;
    s._pad  = 0.0;

    gSnap[idx] = s;
}
