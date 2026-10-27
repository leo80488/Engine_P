// Zeroes the per-cell counters at the start of each flocking frame, one thread per
// cell. cellItems needs no clear: the steer pass reads only cellCount[cell]
// entries.
//
// Root signature (space2):
//   u2 -> DESC_TABLE -> gCellCount UAV

#include "Particle.hlsli"

RWStructuredBuffer<uint> gCellCount : register(u2, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= BOID_GRID_CELLS) return;
    gCellCount[id.x] = 0u;
}
