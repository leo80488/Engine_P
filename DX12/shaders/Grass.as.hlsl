// Grass.as.hlsl - amplification shader: per-patch frustum + distance culling and
// LOD selection for the procedural grass field.
//
// One AS thread = one grass patch (a patchSize x patchSize square of the field
// grid). Each thread:
//   1. frustum-tests the patch AABB (conservative Y range = full terrain amplitude
//      + blade height, the convention Terrain.as.hlsl uses),
//   2. distance-culls against g_cullDist and fades blade density out toward the
//      cull boundary, so patches dissolve instead of popping,
//   3. picks LOD0/1/2 (segment count + blades-per-group + density multiplier),
//   4. computes how many MS groups the patch needs; a group-scope prefix sum
//      (groupshared scan, wave-width agnostic) gives each patch its MS group range
//      in the per-lane GrassPayload.
//
// CPU dispatch count = ceil(patchesPerSide^2 / GRASS_AS_GROUP_SIZE).

#include "Grass.hlsli"

groupshared GrassPayload s_payload;
// Exclusive prefix sum of per-patch MS group counts (+ the total in [32]). A
// groupshared scan, not wave intrinsics, so the result is right for ANY hardware
// wave width: on sub-32-wide hardware a 32-thread group spans several waves, a
// wave prefix sum would restart mid-group and DispatchMesh would go non-uniform.
groupshared uint s_scan[GRASS_AS_GROUP_SIZE + 1];

bool PatchVisible(uint patchIdx, out float dist)
{
    uint  N         = max(g_patchesPerSide, 1u);
    uint  px        = patchIdx % N;
    uint  pz        = patchIdx / N;
    float patchSize = g_grassSize / float(N);
    float2 originXZ = g_grassOrigin + float2(px, pz) * patchSize;
    float2 centerXZ = originXZ + patchSize * 0.5;

    // Distance from the camera to the patch border (not the center), so blades on a
    // patch the camera stands inside are never distance-culled.
    float2 d2 = max(abs(g_cameraPos.xz - centerXZ) - patchSize * 0.5, 0.0);
    dist = length(d2);
    if (dist >= g_cullDist) return false;

    // Conservative AABB: terrain amplitude + blade headroom.
    float3 aabbMin = float3(originXZ.x, g_baseY, originXZ.y);
    float3 aabbMax = float3(originXZ.x + patchSize,
                            g_baseY + g_heightScale + g_bladeHeight * 2.0,
                            originXZ.y + patchSize);

    [unroll]
    for (uint p = 0; p < 6; ++p)
    {
        float4 plane = g_grassFrustumPlanes[p];
        float3 corner;
        corner.x = (plane.x >= 0) ? aabbMax.x : aabbMin.x;
        corner.y = (plane.y >= 0) ? aabbMax.y : aabbMin.y;
        corner.z = (plane.z >= 0) ? aabbMax.z : aabbMin.z;
        if (dot(plane.xyz, corner) + plane.w < 0.0) return false;
    }
    return true;
}

[numthreads(GRASS_AS_GROUP_SIZE, 1, 1)]
void main(uint gtid : SV_GroupThreadID,
          uint gid  : SV_GroupID)
{
    uint N            = max(g_patchesPerSide, 1u);
    uint totalPatches = N * N;
    uint patchIdx     = gid * GRASS_AS_GROUP_SIZE + gtid;

    float dist       = 0.0;
    bool  visible    = (patchIdx < totalPatches) && PatchVisible(patchIdx, dist);

    uint bladeCount = 0;
    uint lod        = 0;
    uint groups     = 0;
    if (visible)
    {
        lod = (dist < g_lod0Dist) ? 0u : ((dist < g_lod1Dist) ? 1u : 2u);

        float patchSize = g_grassSize / float(N);
        float area      = patchSize * patchSize;

        // Density falls to zero across the last 30% of the cull range, with
        // a per-patch hash dither so the dissolve front is not a hard ring.
        float fade   = 1.0 - smoothstep(g_cullDist * 0.70, g_cullDist, dist);
        float dither = GrassHash(patchIdx, 0u, 7u) * 0.15;
        fade = saturate(fade - dither * (1.0 - fade));

        float count = g_density * kGrassDensityMul[lod] * area * fade;
        bladeCount  = min((uint)round(count), (uint)GRASS_MAX_BLADES_PER_PATCH);
        if (bladeCount > 0)
            groups = (bladeCount + kGrassBPG[lod] - 1u) / kGrassBPG[lod];
    }

    // Per-lane payload entries, no compaction: a culled lane has bladeCount 0 -> its
    // MS group range [base, base) is empty and the MS scan can never match it.
    s_payload.patchIdx  [gtid] = patchIdx;
    s_payload.bladeCount[gtid] = bladeCount;
    s_payload.lodLevel  [gtid] = lod;

    // Group-scope exclusive prefix sum of MS group counts: a serial scan on thread 0
    // over 32 entries is trivially cheap and wave-width agnostic.
    s_scan[gtid] = groups;
    GroupMemoryBarrierWithGroupSync();
    if (gtid == 0)
    {
        uint sum = 0;
        [unroll]
        for (uint i = 0; i < GRASS_AS_GROUP_SIZE; ++i)
        {
            uint g    = s_scan[i];
            s_scan[i] = sum;
            sum      += g;
        }
        s_scan[GRASS_AS_GROUP_SIZE] = sum;
    }
    GroupMemoryBarrierWithGroupSync();

    s_payload.groupBase[gtid] = s_scan[gtid];

    DispatchMesh(s_scan[GRASS_AS_GROUP_SIZE], 1, 1, s_payload);
}
