// Bins every live FLOCK agent into the uniform grid. One thread per pool slot:
// reads the stable snapshot (so positions match what the steer pass reads),
// atomically claims a per-cell slot and writes the agent index. Each cell has a
// fixed slice of BOID_MAX_PER_CELL; overflow is dropped (the steer pass clamps its
// read to the cap), which degrades gracefully for flocking.
//
// Root signature (space2):
//   b0 -> BoidParams CBV
//   u1 -> DESC_TABLE -> gSnap      UAV (read)
//   u2 -> DESC_TABLE -> gCellCount UAV (atomic)
//   u3 -> DESC_TABLE -> gCellItems UAV (write)

#include "Particle.hlsli"

cbuffer BoidCB : register(b0, space2)
{
    BoidParams gBoid;
};

RWStructuredBuffer<BoidSnap> gSnap      : register(u1, space2);  // read-only access pattern
RWStructuredBuffer<uint>     gCellCount : register(u2, space2);
RWStructuredBuffer<uint>     gCellItems : register(u3, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // Absolute pool slot = dispatch base + thread id. The value scattered into
    // gCellItems must stay absolute (steer indexes gSnap/gPool with it), and the cell
    // index comes from s.pos, not from idx, so the bias never reaches the grid
    // addressing.
    const uint idx = gBoid.dispatchBase + id.x;
    if (idx >= gBoid.particleCount) return;

    const BoidSnap s = gSnap[idx];
    if (s.valid == 0u) return;   // dead / non-flock: not a grid member

    const int3 c    = BoidCellCoord(s.pos, gBoid.gridOrigin, gBoid.cellSize);
    const uint cell = BoidCellIndex(c);

    uint slot;
    InterlockedAdd(gCellCount[cell], 1u, slot);
    if (slot < BOID_MAX_PER_CELL)
        gCellItems[cell * BOID_MAX_PER_CELL + slot] = idx;
}
