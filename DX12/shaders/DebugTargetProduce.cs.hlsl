// DebugTargetProduce.cs.hlsl - the debug targets no pass writes. The debug
// target view (DebugTargetViewPass::Produce) dispatches one only on the frames a
// view shows its target or a capture names it, right before it paints; each entry
// point writes its own texture.
//
// Compute root signature (space2):
//   b0 - ProduceCB
//   t0 - GBuffer1 (the normal; .a is the 16-bit matIdx integer)    shadingModel, materialCost
//   t1 - the main depth (reverse-Z: exactly 0 where nothing was drawn)
//   t2 - StructuredBuffer<MaterialGPUData> the frame's material table   shadingModel
//   u0 - shadingModel's target (R8_UINT), overdraw's, materialCost's and shadingCost's (R32_UINT)
//   u1 - lightComplexity's target (R16G16B16A16_FLOAT)
//   u2 - mipDensity's target (R16G16_FLOAT)
//   the cluster grids: bindless StructuredBuffer<uint2> handles in the CB
//   the DEBUG_SHADE counters and the material cost table: fixed heap slots
//     (shared/bindless_kind.hlsli BINDLESS_DEBUG_*)

#include "material.hlsli"
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx
#include "core/depth.hlsli"           // LinearizeReverseZ
#include "cluster_common.hlsli"       // GetClusterCoord, ClusterIndex
#include "bindless.hlsli"             // BindlessSBuf

cbuffer ProduceCB : register(b0, space2)
{
    uint2 Size;            // the render size (the GBuffer's)
    uint  MaterialCount;   // g_materials' element count
    uint  GridFlags;       // lightComplexity: the grids this frame wrote - 1 lights, 2 decals, 4 probes
    float NearZ;           // the camera's = the cluster grid's
    float FarZ;
    uint  Orthographic;
    uint  Coverage;        // materialCost: RWStructuredBuffer<uint> handle, the visible pixels per slot (0: none)
    uint  LightGrid;       // {offset, count} per cluster (ClusterPass; offset ~0 = the list overflowed)
    uint  DecalGrid;       // (DecalPass)
    uint  ProbeGrid;       // (ClusterPass's probe cull)
    uint  _pad1;
};

Texture2D<float4>                 g_normal     : register(t0, space2);
Texture2D<float>                  g_depth      : register(t1, space2);
StructuredBuffer<MaterialGPUData> g_materials  : register(t2, space2);
RWTexture2D<uint>                 g_uintTarget : register(u0, space2);
RWTexture2D<float4>               g_complexity : register(u1, space2);
RWTexture2D<float2>               g_density    : register(u2, space2);

static const uint kGridLights = 1u;
static const uint kGridDecals = 2u;
static const uint kGridProbes = 4u;

// shadingModel: the pixel's material's ShadingModel (Graphics/ShadingModel.h,
// 0..7); 255 where nothing was drawn, 254 for a matIdx past the table.
[numthreads(8, 8, 1)]
void CSShadingModel(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size.x || id.y >= Size.y) return;
    if (g_depth.Load(int3(id.xy, 0)) <= 0.0)
    {
        g_uintTarget[id.xy] = 255u;
        return;
    }
    const uint matIdx = GBufferDecodeMatIdx(g_normal.Load(int3(id.xy, 0)).a);
    g_uintTarget[id.xy] = matIdx < MaterialCount ? g_materials[matIdx].customShadingModel : 254u;
}

// materialCost: the instruction count of the pixel shader that drew the visible
// opaque surface - the material cost table (BINDLESS_DEBUG_COST_TABLE, filled this
// frame by Renderer::CollectMaterialCosts) at GBuffer1.a's matIdx; 0 where nothing
// was drawn or past the table.
[numthreads(8, 8, 1)]
void CSMaterialCost(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size.x || id.y >= Size.y) return;
    uint cost = 0u;
    if (g_depth.Load(int3(id.xy, 0)) > 0.0)
    {
        const uint matIdx = GBufferDecodeMatIdx(g_normal.Load(int3(id.xy, 0)).a);
        if (matIdx < MaterialCount)
        {
            cost = BindlessSBuf<uint>(BINDLESS_DEBUG_COST_TABLE)[matIdx];
            // The report's coverage: one more visible pixel of the slot - one atomic per
            // wave where the wave's pixels share it, as they mostly do.
            if (Coverage != 0u)
            {
                RWStructuredBuffer<uint> pixels = BindlessRWSBuf<uint>(Coverage);
                if (WaveActiveAllEqual(matIdx))
                {
                    const uint n = WaveActiveCountBits(true);
                    if (WaveIsFirstLane()) InterlockedAdd(pixels[matIdx], n);
                }
                else
                    InterlockedAdd(pixels[matIdx], 1u);
            }
        }
    }
    g_uintTarget[id.xy] = cost;
}

// shadingCost: per pixel, the sum of the cost table entries of every fragment the
// material pixel shaders ran for (debug_shade.hlsli accumulates it into the
// R32_UINT at BINDLESS_DEBUG_COST in the DEBUG_SHADE permutation); copied out and
// zeroed for the next frame.
[numthreads(8, 8, 1)]
void CSShadingCost(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size.x || id.y >= Size.y) return;
    RWTexture2D<uint> sum = BindlessRWTex2D<uint>(BINDLESS_DEBUG_COST);
    g_uintTarget[id.xy] = sum[id.xy];
    sum[id.xy] = 0u;
}

// overdraw: the fragments the material pixel shaders ran for this frame
// (debug_shade.hlsli counts each into the R32_UINT at BINDLESS_DEBUG_OVERDRAW in
// the DEBUG_SHADE permutation); copied out and zeroed for the next frame.
[numthreads(8, 8, 1)]
void CSOverdraw(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size.x || id.y >= Size.y) return;
    RWTexture2D<uint> count = BindlessRWTex2D<uint>(BINDLESS_DEBUG_OVERDRAW);
    g_uintTarget[id.xy] = count[id.xy];
    count[id.xy] = 0u;
}

// mipDensity: the front-most material fragment's base colour texture density.
// debug_shade.hlsli packs it under the depth into the R32_UINT at
// BINDLESS_DEBUG_DENSITY with InterlockedMax; unpacked here: r log2 texels per
// pixel, g 1 where that fragment was textured (r -1000, g 0 where it was not or
// nothing was drawn). The counter is zeroed for the next frame.
[numthreads(8, 8, 1)]
void CSDensity(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size.x || id.y >= Size.y) return;
    RWTexture2D<uint> packed = BindlessRWTex2D<uint>(BINDLESS_DEBUG_DENSITY);
    const uint code = packed[id.xy] & 0xFFu;
    packed[id.xy] = 0u;
    g_density[id.xy] = code != 0u ? float2((float)code / 16.0 - 8.0, 1.0) : float2(-1000.0, 0.0);
}

// lightComplexity: the lists of the pixel's cluster, as Lighting.ps / DecalApply
// pick it (GetClusterCoord at the pixel centre and the view depth) - r the local
// lights (every light where the list overflowed), g the decals, b the reflection
// probes, a 1 where the light list overflowed. 0 where nothing was drawn, and in
// a channel whose grid this frame did not write (no light / decal / probe: the
// buffer still holds an older frame's).
[numthreads(8, 8, 1)]
void CSLightComplexity(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size.x || id.y >= Size.y) return;
    const float d = g_depth.Load(int3(id.xy, 0));
    if (d <= 0.0)
    {
        g_complexity[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    // Reverse-Z: 1 at the near plane, 0 at the far (an orthographic depth is linear).
    const float viewZ = Orthographic != 0u ? lerp(FarZ, NearZ, d) : LinearizeReverseZ(d, NearZ, FarZ);
    const float2 uv = (float2(id.xy) + 0.5) / float2(Size);
    const uint key = ClusterIndex(GetClusterCoord(uv, viewZ, NearZ, FarZ));

    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    if ((GridFlags & kGridLights) != 0u)
    {
        const uint2 e = BindlessSBuf<uint2>(LightGrid)[key];
        o.r = (float)e.y;
        o.a = e.x == 0xFFFFFFFFu ? 1.0 : 0.0;
    }
    if ((GridFlags & kGridDecals) != 0u) o.g = (float)BindlessSBuf<uint2>(DecalGrid)[key].y;
    if ((GridFlags & kGridProbes) != 0u) o.b = (float)BindlessSBuf<uint2>(ProbeGrid)[key].y;
    g_complexity[id.xy] = o;
}
