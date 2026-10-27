#ifndef WORLD_LIGHT_GRID_HLSLI
#define WORLD_LIGHT_GRID_HLSLI
// CPU mirror: ClusterPass::UpdateWorldLightGrid, 8-word header, 32x16x32,
// 2 words/cell (count, absolute list offset), followed by compact light IDs.
// Sphere AABBs conservatively cover spot cones too; pool exhaustion falls back.
uint WorldLightRange(uint heapIndex, float3 P, uint total, out uint offset)
{
    offset = 0xffffffffu;
    if (heapIndex == 0xffffffffu) return total;
    StructuredBuffer<uint> grid = ResourceDescriptorHeap[heapIndex];
    uint state = grid[0];
    if (state == 0u || any(isnan(P)) || any(isinf(P))) return total;
    if (state == 2u) return 0u;
    float3 lo = asfloat(uint3(grid[1],grid[2],grid[3]));
    float3 inv = asfloat(uint3(grid[4],grid[5],grid[6]));
    float3 c = floor((P-lo)*inv);
    if (any(c < 0) || any(c >= float3(32,16,32))) return 0u;
    uint3 cell = (uint3)c;
    uint base = 8u + ((cell.z*16u+cell.y)*32u+cell.x)*2u;
    uint count = grid[base];
    if (count == 0xffffffffu) return total;
    offset = grid[base+1u];
    return count;
}
uint WorldLightIndex(uint heapIndex, uint offset, uint i)
{
    if (offset == 0xffffffffu) return i;
    StructuredBuffer<uint> grid = ResourceDescriptorHeap[heapIndex];
    return grid[offset+i];
}
#endif
