// Terrain amplification shader for the colour pass: per-sub-tile frustum culling.
// One AS thread group = AS_GROUP_SIZE sub-tiles. Each thread tests one sub-tile's
// world-space AABB against the six camera frustum planes in TerrainCB (the shadow
// pass culls per cascade in Terrain.shadow.as.hlsl); the group then emits one MS
// group per visible sub-tile via DispatchMesh, with a payload carrying the sub-tile
// index so the MS can rebuild (subX, subZ).
// The CPU dispatches ceil(tilesPerSide^2 / AS_GROUP_SIZE) groups: tilesPerSide =
// 128 gives 512 AS groups, each emitting 0..32 MS groups.
// Conservative AABB Y range: [worldCenterY, worldCenterY + heightScale]. A min / max
// heightmap pyramid would tighten it (most sub-tiles span a fraction of that
// range); the conservative box already culls well when looking sideways or up.

#define AS_GROUP_SIZE 32

// The terrain constant buffer (b2): shared/terrain_params.hlsli, the one definition
// C++ fills too. The colour AS is the only stage that reads frustumPlanes.
#include "shared/terrain_params.hlsli"
ConstantBuffer<TerrainParams> g_terrain : register(b2, space0);

struct TerrainPayload
{
    uint subTileIdx[AS_GROUP_SIZE];
};

groupshared TerrainPayload s_payload;
// Exclusive prefix sum of the per-thread visible flag (+ total in [32]).
// A group-scope scan in groupshared rather than wave intrinsics, so the result is
// right for any wave width: wave ops are per wave, and where a 32-thread group spans
// several waves the prefix sums would restart mid-group and DispatchMesh would go
// non-uniform.
groupshared uint s_scan[AS_GROUP_SIZE + 1];

// AABB-vs-frustum p-vertex test. True when the box is at least partly inside
// every plane (conservative: no false negatives, may pass boxes that only touch
// the outside).
bool SubTileVisible(uint subTileIdx)
{
    if (g_terrain.enableFrustumCull == 0) return true;

    uint N = max(g_terrain.tilesPerSide, 1u);
    uint subX = subTileIdx % N;
    uint subZ = subTileIdx / N;

    float subTileSize = g_terrain.worldSize / float(N);
    float2 originXZ   = g_terrain.worldOrigin + float2(subX, subZ) * subTileSize;

    float3 aabbMin = float3(originXZ.x,                 g_terrain.worldCenterY,                  originXZ.y);
    float3 aabbMax = float3(originXZ.x + subTileSize,   g_terrain.worldCenterY + g_terrain.heightScale,  originXZ.y + subTileSize);

    [unroll]
    for (uint p = 0; p < 6; ++p)
    {
        float4 plane = g_terrain.frustumPlanes[p];
        // P-vertex: pick the corner most likely to be INSIDE the plane.
        // (For inward-normal planes that's the corner along +normal.)
        float3 corner;
        corner.x = (plane.x >= 0) ? aabbMax.x : aabbMin.x;
        corner.y = (plane.y >= 0) ? aabbMax.y : aabbMin.y;
        corner.z = (plane.z >= 0) ? aabbMax.z : aabbMin.z;
        // dot(plane.xyz, corner) + plane.w < 0 -> the whole box is outside this plane.
        if (dot(plane.xyz, corner) + plane.w < 0.0) return false;
    }
    return true;
}

[numthreads(AS_GROUP_SIZE, 1, 1)]
void main(uint gtid : SV_GroupThreadID,
          uint gid  : SV_GroupID)
{
    uint N             = max(g_terrain.tilesPerSide, 1u);
    uint totalSubTiles = N * N;
    uint subTileIdx    = gid * AS_GROUP_SIZE + gtid;

    bool visible = (subTileIdx < totalSubTiles) && SubTileVisible(subTileIdx);

    // Group-scope stream compaction: exclusive prefix sum of the visible
    // flag gives each surviving thread its slot in the payload, and the
    // total is the (group-uniform) MS group count. A serial scan on thread 0
    // over 32 entries is trivially cheap and wave-width agnostic.
    s_scan[gtid] = visible ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    if (gtid == 0)
    {
        uint sum = 0;
        [unroll]
        for (uint i = 0; i < AS_GROUP_SIZE; ++i)
        {
            uint v    = s_scan[i];
            s_scan[i] = sum;
            sum      += v;
        }
        s_scan[AS_GROUP_SIZE] = sum;
    }
    GroupMemoryBarrierWithGroupSync();

    if (visible)
        s_payload.subTileIdx[s_scan[gtid]] = subTileIdx;

    DispatchMesh(s_scan[AS_GROUP_SIZE], 1, 1, s_payload);
}
