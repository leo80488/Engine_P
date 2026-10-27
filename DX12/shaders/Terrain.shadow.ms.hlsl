// Terrain shadow mesh shader, depth only.
// The multi-tile heightmap geometry of Terrain.ms.hlsl, projected with the cascade's light VP (b1)
// and emitting only SV_Position (null PS). ShadowPass dispatches it once per cascade after the
// regular draws, into the Texture2DArray slice the scene's other casters wrote.

#include "Terrain.hlsli"

#define GRID_DIM      12
#define VERT_COUNT    (GRID_DIM * GRID_DIM)
#define PRIM_COUNT    ((GRID_DIM - 1) * (GRID_DIM - 1) * 2)
#define THREAD_COUNT  64
#define AS_GROUP_SIZE 32                                  // must match Terrain.shadow.as.hlsl

struct TerrainPayload
{
    uint subTileIdx[AS_GROUP_SIZE];
};

// Per-cascade shadow VP, bound by ShadowPass at b1 space0; layout as Shadow.vs.hlsl's
// ShadowPerViewCB.
cbuffer ShadowPerViewCB : register(b1, space0)
{
    float4x4 g_shadowVP;
};

// The terrain constant buffer (b2): shared/terrain_params.hlsli is the one definition C++ fills too.
// The colour and shadow passes share it; this MS reads the geometry fields.
#include "shared/terrain_params.hlsli"
ConstantBuffer<TerrainParams> g_terrain : register(b2, space0);

Texture2D<float>  g_HeightMap   : register(t2, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_LinearClamp SmpFixed(SMP_LINEAR_CLAMP)

// The same height function as Terrain.ms.hlsl, so both passes produce identical vertex positions
// (no z-fighting at silhouettes).
float SampleHeight(float2 uv)
{
    if (g_terrain.hasHeightmap == 0) return g_terrain.worldCenterY;
    float h = TerrainSampleHeightBicubic(g_HeightMap, g_LinearClamp, uv, g_terrain.heightmapTexel);
    return g_terrain.worldCenterY + h * g_terrain.heightScale;
}

struct VertexOut
{
    float4 sv : SV_Position;
};

[outputtopology("triangle")]
[numthreads(THREAD_COUNT, 1, 1)]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    in   payload TerrainPayload payload,
    out  vertices VertexOut verts[VERT_COUNT],
    out  indices  uint3     tris [PRIM_COUNT])
{
    SetMeshOutputCounts(VERT_COUNT, PRIM_COUNT);

    const uint  subTileIdx  = payload.subTileIdx[gid];
    const uint  N           = max(g_terrain.tilesPerSide, 1u);
    const uint  subX        = subTileIdx % N;
    const uint  subZ        = subTileIdx / N;
    const float macroSize   = g_terrain.worldSize;
    const float subTileSize = macroSize / float(N);
    const float2 tileOrigin = g_terrain.worldOrigin + float2(subX, subZ) * subTileSize;
    const float  step       = subTileSize / float(GRID_DIM - 1);

    [unroll(4)]
    for (uint v = gtid; v < VERT_COUNT; v += THREAD_COUNT)
    {
        uint2  gridXY     = uint2(v % GRID_DIM, v / GRID_DIM);
        float2 worldXZ    = tileOrigin + float2(gridXY) * step;
        float2 globalNorm = (worldXZ - g_terrain.worldOrigin) / macroSize;
        float2 hmUV       = g_terrain.heightmapUVOffset + globalNorm * g_terrain.heightmapUVScale;

        float  h        = SampleHeight(hmUV);
        float3 worldPos = float3(worldXZ.x, h, worldXZ.y);

        VertexOut o;
        o.sv = mul(float4(worldPos, 1.0), g_shadowVP);
        verts[v] = o;
    }

    [unroll(4)]
    for (uint p = gtid; p < PRIM_COUNT; p += THREAD_COUNT)
    {
        uint quadIdx = p / 2;
        uint quadX   = quadIdx % (GRID_DIM - 1);
        uint quadY   = quadIdx / (GRID_DIM - 1);

        uint i0 = quadY * GRID_DIM + quadX;
        uint i1 = i0 + 1;
        uint i2 = i0 + GRID_DIM;
        uint i3 = i2 + 1;

        // The colour pass's winding, so BACK culling drops the same faces; the ShadowPass PSO's depth
        // bias handles acne.
        tris[p] = (p & 1) ? uint3(i1, i2, i3) : uint3(i0, i2, i1);
    }
}
