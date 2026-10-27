// Reynolds Boids flocking for FLOCK particles, accelerated by a uniform spatial grid.
//
// One thread per pool slot; non-FLOCK / dead slots early-out. Each boid scans the 3x3x3
// grid cells around its own cell (built this frame by ParticleBoidGridBuild from the stable
// snapshot), so the cost is O(neighbours), not O(slice). cellSize == perceptionRadius, so a
// 3x3x3 block covers the perception sphere. Applies the three steering rules plus soft
// spherical containment, integrates velocity (clamped to maxSpeed) and position, re-applies
// the emitter's live appearance and re-arms lifetime (the flock is immortal).
// ParticleUpdate.cs.hlsl skips these particles.
//
// Root signature (space2):
//   b0 -> BoidParams CBV
//   u0 -> DESC_TABLE -> gPool      UAV (read/write)
//   u1 -> DESC_TABLE -> gSnap      UAV (read-only neighbour source)
//   u2 -> DESC_TABLE -> gCellCount UAV (read)
//   u3 -> DESC_TABLE -> gCellItems UAV (read)

#include "Particle.hlsli"

cbuffer BoidCB : register(b0, space2)
{
    BoidParams gBoid;
};

RWStructuredBuffer<Particle> gPool      : register(u0, space2);
RWStructuredBuffer<BoidSnap> gSnap      : register(u1, space2);  // read-only access pattern
RWStructuredBuffer<uint>     gCellCount : register(u2, space2);  // read-only access pattern
RWStructuredBuffer<uint>     gCellItems : register(u3, space2);  // read-only access pattern

// Reynolds steering: accelerate from current velocity toward the desired
// direction (at full cruise speed), with the steering force capped to maxForce.
float3 SteerToward(float3 target, float3 vel, float maxSpeed, float maxForce)
{
    const float len = length(target);
    if (len < 1e-5) return float3(0, 0, 0);
    const float3 desired = target * (maxSpeed / len);
    float3 steer = desired - vel;
    const float sl = length(steer);
    if (sl > maxForce) steer *= (maxForce / sl);
    return steer;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // Absolute pool slot = dispatch base + thread id. Must stay absolute: it is compared
    // against gCellItems entries and gives the slot's own slice base below.
    const uint idx = gBoid.dispatchBase + id.x;
    if (idx >= gBoid.particleCount) return;

    Particle p = gPool[idx];
    if ((p.flags & PARTICLE_FLAG_FLOCK) == 0u) return;  // not a boid
    if (p.lifetime <= 0.0) return;                       // dead slot in the slice

    // Kill agents past the live population (handles a shrunk flockCount without a
    // re-spawn). The flock occupies [sliceBase, sliceBase + flockCount).
    const uint sliceLen  = max(gBoid.sliceLen, 1u);
    const uint sliceBase = (idx / sliceLen) * sliceLen;
    if (idx - sliceBase >= gBoid.flockCount)
    {
        p.lifetime = 0.0;
        p.flags    = 0u;            // un-flag so it's a normal dead slot
        gPool[idx] = p;
        return;
    }

    const float3 pos = p.position;
    float3       vel = p.velocity;

    const float perc2 = gBoid.perceptionRadius * gBoid.perceptionRadius;
    const float sepr2 = gBoid.separationRadius * gBoid.separationRadius;

    float3 cohSum = float3(0, 0, 0);   // sum of neighbour positions  (cohesion)
    float3 aliSum = float3(0, 0, 0);   // sum of neighbour velocities (alignment)
    float3 sepSum = float3(0, 0, 0);   // accumulated push-away vector (separation)
    uint   cohN = 0, aliN = 0, sepN = 0;

    // Scan the 3x3x3 block of grid cells around this boid.
    const int3 c0 = BoidCellCoord(pos, gBoid.gridOrigin, gBoid.cellSize);
    for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        const int3 c = c0 + int3(dx, dy, dz);
        if (any(c < 0) || any(c >= (int)BOID_GRID_DIM)) continue;

        const uint cell = BoidCellIndex(c);
        const uint cnt  = min(gCellCount[cell], BOID_MAX_PER_CELL);
        const uint base = cell * BOID_MAX_PER_CELL;

        for (uint k = 0; k < cnt; ++k)
        {
            const uint j = gCellItems[base + k];
            if (j == idx) continue;
            const BoidSnap nb = gSnap[j];   // only valid agents are binned
            const float3 d     = nb.pos - pos;
            const float  dist2 = dot(d, d);
            if (dist2 > perc2 || dist2 < 1e-8) continue;

            cohSum += nb.pos; ++cohN;
            aliSum += nb.vel; ++aliN;
            if (dist2 < sepr2)
            {
                sepSum -= d / dist2;   // closer = stronger push
                ++sepN;
            }
        }
    }

    float3 accel = float3(0, 0, 0);
    if (cohN > 0)
    {
        const float3 toCenter = cohSum / (float)cohN - pos;
        accel += SteerToward(toCenter, vel, gBoid.maxSpeed, gBoid.maxForce) * gBoid.cohesionWeight;
    }
    if (aliN > 0)
    {
        const float3 avgVel = aliSum / (float)aliN;
        accel += SteerToward(avgVel, vel, gBoid.maxSpeed, gBoid.maxForce) * gBoid.alignmentWeight;
    }
    if (sepN > 0)
    {
        accel += SteerToward(sepSum, vel, gBoid.maxSpeed, gBoid.maxForce) * gBoid.separationWeight;
    }

    // Soft spherical containment around the flock home so it doesn't drift off.
    if (gBoid.boundsWeight > 0.0)
    {
        const float3 toHome = gBoid.boundsCenter - pos;
        if (dot(toHome, toHome) > gBoid.boundsRadius * gBoid.boundsRadius)
            accel += SteerToward(toHome, vel, gBoid.maxSpeed, gBoid.maxForce) * gBoid.boundsWeight;
    }

    vel += accel * gBoid.deltaTime;

    // Clamp into [0.25, 1.0] * maxSpeed so boids keep cruising instead of stalling.
    const float spd = length(vel);
    if (spd > gBoid.maxSpeed)
        vel *= (gBoid.maxSpeed / spd);
    else if (spd > 1e-4 && spd < gBoid.maxSpeed * 0.25)
        vel *= (gBoid.maxSpeed * 0.25 / spd);

    p.position = pos + vel * gBoid.deltaTime;
    p.velocity = vel;
    p.lifetime = p.maxLifetime;   // keep the flock immortal

    // Re-apply the emitter's current appearance so inspector edits are live on a
    // one-shot (never re-spawned) flock.
    p.startColor         = gBoid.startColor;
    p.startScale         = gBoid.startScale;
    p.startSize          = gBoid.startSize;
    p.intensity          = gBoid.intensity;
    p.visualMode         = gBoid.visualMode;
    p.orientation        = gBoid.orientation;
    p.textureBindlessIdx = gBoid.textureBindlessIdx;

    gPool[idx] = p;
}
