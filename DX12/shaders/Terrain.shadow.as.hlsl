// Terrain shadow amplification shader: per-cascade frustum cull.
// Each thread tests one sub-tile's world AABB against the 6 planes of the CURRENT cascade
// (ShadowPerViewCB at b1, uploaded by ShadowPass per cascade); survivors are group-compacted and
// sent to the depth-only MS. Separate from Terrain.as.hlsl, which culls against the CAMERA frustum:
// that would drop sub-tiles the light sees but the camera does not and break self-shadowing at
// glancing angles.

#define AS_GROUP_SIZE 32

// ShadowPerViewCB, written by ShadowPass per cascade: the VP (used by the MS) and the cascade's
// frustum planes (used here).
cbuffer ShadowPerViewCB : register(b1, space0)
{
    float4x4 _g_shadowVP;
    float4   g_shadowFrustumPlanes[6];
};

// The terrain constant buffer (b2): shared/terrain_params.hlsli is the one definition C++ fills too.
// This AS culls against the cascade planes in b1, not frustumPlanes here.
#include "shared/terrain_params.hlsli"
ConstantBuffer<TerrainParams> g_terrain : register(b2, space0);

struct TerrainPayload
{
    uint subTileIdx[AS_GROUP_SIZE];
};

groupshared TerrainPayload s_payload;
// Exclusive prefix sum of the visible flags, total in [AS_GROUP_SIZE]. Scanned in groupshared, not
// with wave intrinsics, so it is right for any wave width: on hardware with waves narrower than 32
// the group spans several waves, a wave prefix sum would restart mid-group and DispatchMesh would go
// non-uniform.
groupshared uint s_scan[AS_GROUP_SIZE + 1];

// AABB-vs-frustum p-vertex test against the per-cascade planes.
bool SubTileVisible(uint subTileIdx)
{
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
        float4 plane = g_shadowFrustumPlanes[p];
        float3 corner;
        corner.x = (plane.x >= 0) ? aabbMax.x : aabbMin.x;
        corner.y = (plane.y >= 0) ? aabbMax.y : aabbMin.y;
        corner.z = (plane.z >= 0) ? aabbMax.z : aabbMin.z;
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

    // Group-scope stream compaction: the exclusive prefix sum gives each surviving thread its payload
    // slot and the total is the group-uniform MS group count. A serial scan of 32 entries on thread 0
    // is cheap and wave-width agnostic.
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
