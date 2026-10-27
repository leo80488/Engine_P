// Builds the per-cluster view-space AABBs.
// Dispatch ceil(CLUSTER_TILE_X/4, CLUSTER_TILE_Y/4, CLUSTER_SLICES/4) groups of 4x4x4, so one
// group fills a wave; SV_DispatchThreadID maps 1:1 to (tileX, tileY, slice) and the bounds
// check drops the lanes that overhang the grid.

#include "cluster_common.hlsli"

cbuffer ClusterCB : register(b0, space2)
{
    float4x4 invProj;     // inverse projection matrix (row-vector convention)
    float    nearZ;
    float    farZ;
    uint     screenW;
    uint     screenH;
    uint     lightCount;
    uint     probeCount;   // ClusterCullProbes reads this slot; unused here
    uint     isOrtho;      // 1 = orthographic projection (editor axis views)
    uint     probeBufferIdx; // ClusterCullProbes reads this slot; unused here
};

RWStructuredBuffer<ClusterAABB> g_ClusterAABBs : register(u0, space2);

// Screen-space UV + NDC depth -> view-space position.
float3 ScreenToView(float2 uv, float ndcDepth)
{
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndcDepth, 1.0);
    float4 viewPos = mul(ndc, invProj);
    return viewPos.xyz / viewPos.w;
}

// View-space depth at the start of a Z slice (exponential distribution).
float SliceDepth(uint slice)
{
    return nearZ * pow(farZ / nearZ, float(slice) / float(CLUSTER_SLICES));
}

[numthreads(4, 4, 4)]
void CSBuildClusters(uint3 id : SV_DispatchThreadID)
{
    uint tileX = id.x;
    uint tileY = id.y;
    uint slice = id.z;

    if (tileX >= CLUSTER_TILE_X || tileY >= CLUSTER_TILE_Y || slice >= CLUSTER_SLICES)
        return;

    // Screen-space UV bounds for this tile.
    float2 uvMin = float2(float(tileX) / CLUSTER_TILE_X,
                          float(tileY) / CLUSTER_TILE_Y);
    float2 uvMax = float2(float(tileX + 1) / CLUSTER_TILE_X,
                          float(tileY + 1) / CLUSTER_TILE_Y);

    // Depth range for this slice (view-space Z, positive = in front of the camera).
    float zNear = SliceDepth(slice);
    float zFar  = SliceDepth(slice + 1);

    // View-space Z -> NDC depth. The engine is reversed-Z (XMMatrixPerspectiveFovLH with near and
    // far swapped): ndc.z = nearZ * (z - farZ) / (z * (nearZ - farZ)), so z = nearZ -> 1 and
    // z = farZ -> 0. Do not use the forward-Z formula: invProj then unprojects to the wrong Z and
    // the AABBs miss the frustum (lights pop between mis-sized clusters).
    float ndcNear, ndcFar;
    if (isOrtho != 0)
    {
        // Reversed-Z ortho is affine: z = nearZ -> ndc 1, z = farZ -> ndc 0.
        ndcNear = (zNear - farZ) / (nearZ - farZ);
        ndcFar  = (zFar  - farZ) / (nearZ - farZ);
    }
    else
    {
        ndcNear = nearZ * (zNear - farZ) / (zNear * (nearZ - farZ));
        ndcFar  = nearZ * (zFar  - farZ) / (zFar  * (nearZ - farZ));
    }

    // The tile's 8 frustum corners in view space -> AABB.
    float3 corners[8];
    corners[0] = ScreenToView(float2(uvMin.x, uvMin.y), ndcNear);
    corners[1] = ScreenToView(float2(uvMax.x, uvMin.y), ndcNear);
    corners[2] = ScreenToView(float2(uvMin.x, uvMax.y), ndcNear);
    corners[3] = ScreenToView(float2(uvMax.x, uvMax.y), ndcNear);
    corners[4] = ScreenToView(float2(uvMin.x, uvMin.y), ndcFar);
    corners[5] = ScreenToView(float2(uvMax.x, uvMin.y), ndcFar);
    corners[6] = ScreenToView(float2(uvMin.x, uvMax.y), ndcFar);
    corners[7] = ScreenToView(float2(uvMax.x, uvMax.y), ndcFar);

    float3 aabbMin = corners[0];
    float3 aabbMax = corners[0];
    [unroll]
    for (uint i = 1; i < 8; ++i)
    {
        aabbMin = min(aabbMin, corners[i]);
        aabbMax = max(aabbMax, corners[i]);
    }

    uint idx = ClusterIndex(uint3(tileX, tileY, slice));
    g_ClusterAABBs[idx].minPt = aabbMin;
    g_ClusterAABBs[idx].maxPt = aabbMax;
}
