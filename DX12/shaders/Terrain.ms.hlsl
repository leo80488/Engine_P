// Terrain mesh shader (multi-tile dispatch).
// One thread group emits one sub-tile of the tilesPerSide x tilesPerSide macro grid: 12x12 = 144
// verts / 242 prims, under the 256/256 MS limits. The heightmap is sampled bicubically
// (Terrain.hlsli), so the displacement is C2 even for BC-compressed or 8-bit sources.

#include "Terrain.hlsli"

#define GRID_DIM      12
#define VERT_COUNT    (GRID_DIM * GRID_DIM)                  // 144
#define PRIM_COUNT    ((GRID_DIM - 1) * (GRID_DIM - 1) * 2)  // 242
#define THREAD_COUNT  64
#define AS_GROUP_SIZE 32                                     // must match Terrain.as.hlsl

// Payload from Terrain.as.hlsl: the sub-tiles that survived the AS frustum cull, compacted; gid
// indexes this list.
struct TerrainPayload
{
    uint subTileIdx[AS_GROUP_SIZE];
};

// ==== Bindings ====

cbuffer PerViewCB : register(b1, space0)
{
    // Mirrors the Renderer's PerView CB layout (as GBuffer.vs.hlsl) to reuse its root-CBV slot. The MS
    // projects with viewProj and writes the unjittered current / previous clip positions the PS turns
    // into velocity.
    float4x4 g_viewProj;
    float4x4 g_prevViewProj;
    float4x4 g_curViewProjNoJitter;
};

// The terrain constant buffer (b2): shared/terrain_params.hlsli is the one definition C++ fills too.
// The MS reads the geometry fields.
#include "shared/terrain_params.hlsli"
ConstantBuffer<TerrainParams> g_terrain : register(b2, space0);

Texture2D<float>  g_HeightMap   : register(t2, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_LinearClamp SmpFixed(SMP_LINEAR_CLAMP)

// ==== Output struct ====

// No tangent frame: Terrain.ps.hlsl re-derives N/T/B per pixel from the heightmap
// (ComputeTerrainTBN), so an interpolated one would cost 4 bicubic taps per vertex for nothing.
struct VertexOut
{
    float4 sv        : SV_Position;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float4 curClip   : TEXCOORD1;
    float4 prevClip  : TEXCOORD2;
};

// ==== Heightmap sampling ====

// Returns world-space Y for a heightmap UV. g_terrain.worldCenterY is the terrain's BASE: height 0
// maps to it and 1 to worldCenterY + heightScale, so heightScale raises only the peaks. Bicubic
// B-spline filtering keeps the surface C2 even for a quantised heightmap (no terracing).
float SampleHeight(float2 uv)
{
    if (g_terrain.hasHeightmap == 0) return g_terrain.worldCenterY;
    float h = TerrainSampleHeightBicubic(g_HeightMap, g_LinearClamp, uv, g_terrain.heightmapTexel);
    return g_terrain.worldCenterY + h * g_terrain.heightScale;
}

// ==== Main ====

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

    // One group = one visible sub-tile: gid in [0, visibleCount) -> original sub-tile index from the
    // compacted payload -> (subX, subZ) via g_terrain.tilesPerSide.
    const uint  subTileIdx  = payload.subTileIdx[gid];
    const uint  N           = max(g_terrain.tilesPerSide, 1u);
    const uint  subX        = subTileIdx % N;
    const uint  subZ        = subTileIdx / N;
    const float macroSize   = g_terrain.worldSize;
    const float subTileSize = macroSize / float(N);

    // Bottom-left corner of THIS sub-tile in world XZ.
    const float2 tileOrigin = g_terrain.worldOrigin + float2(subX, subZ) * subTileSize;
    // Grid step inside the sub-tile; neighbouring sub-tiles get the same world XZ on their shared edge,
    // so there are no T-junctions.
    const float  step       = subTileSize / float(GRID_DIM - 1);

    // Vertices: each thread writes a strided subset.
    [unroll(4)]
    for (uint v = gtid; v < VERT_COUNT; v += THREAD_COUNT)
    {
        uint2 gridXY = uint2(v % GRID_DIM, v / GRID_DIM);
        float2 worldXZ = tileOrigin + float2(gridXY) * step;

        // Heightmap UV from the GLOBAL macro-tile position, so every sub-tile samples its own slice of the
        // one heightmap.
        float2 globalNorm = (worldXZ - g_terrain.worldOrigin) / macroSize;
        float2 hmUV       = g_terrain.heightmapUVOffset + globalNorm * g_terrain.heightmapUVScale;

        float h = SampleHeight(hmUV);
        float3 worldPos = float3(worldXZ.x, h, worldXZ.y);

        VertexOut o;
        o.sv       = mul(float4(worldPos, 1.0), g_viewProj);
        o.worldPos = worldPos;
        o.uv       = hmUV;
        o.curClip  = mul(float4(worldPos, 1.0), g_curViewProjNoJitter);
        o.prevClip = mul(float4(worldPos, 1.0), g_prevViewProj);

        verts[v] = o;
    }

    // Indices: each thread emits a strided subset of quads.
    // The default rasterizer state is CW = front with BACK culling. With the vertex layout in world XZ
    //   i0=(X,  Z  )   i1=(X+1, Z  )
    //   i2=(X,  Z+1)   i3=(X+1, Z+1)
    // the order (i0, i2, i1) gives a +Y geometric normal, visible from above.
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

        // Both triangles wound so the geometric normal is +Y (top facing up).
        tris[p] = (p & 1) ? uint3(i1, i2, i3) : uint3(i0, i2, i1);
    }
}
