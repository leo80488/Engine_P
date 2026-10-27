// SceneVoxelClear.cs.hlsl - zeroes the 3D occupancy texture at the start of each voxelize frame,
// before the per-mesh triangle dispatches, so they only WRITE 255 (no read-modify-write atomics).
// Dispatch: ceil(dim/8)^3 groups of 8^3 threads.

cbuffer ClearCB : register(b0, space2)
{
    uint  gridDim;
    uint3 _pad;
};

RWTexture3D<uint> gOccupancy : register(u0, space2);

[numthreads(8, 8, 8)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (any(dtid >= gridDim)) return;
    gOccupancy[dtid] = 0u;
}
