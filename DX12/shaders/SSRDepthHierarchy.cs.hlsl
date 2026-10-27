// SSRDepthHierarchy.cs.hlsl - 2-channel depth pyramid for SSR Hi-Z traversal.
// A port of Wicked Engine's ssr_depthHierarchyCS.hlsl (the FidelityFX SSSR approach): per texel
// (maxDepth, minDepth), so the march can pick the tile back wall for its direction and skip whole
// tiles at coarse mips. Reverse-Z (z_ndc 1 = near, 0 = far): .r = max = NEAREST surface in the
// tile, .g = min = FARTHEST. Wicked's march is reverse-Z too and samples .r.
//
// Entry points (one CB layout; shared compute root signature, CreateComputeRootSignature):
//   CSMain_Mip0   : depth SRV t0 -> u0 (mip 0)
//   CSMain_Reduce : previous mip u0 -> u1 (mip N) + u2 (mip N+1, unwritten when writeSecond == 0,
//                   the odd tail); the reduction core is depth_pyramid_reduce.hlsli, shared with Hi-Z.
// Root slots: [0] CBV b0, [1] SRV t0, [4] UAV u0, [5] UAV u1, [14] UAV u2 (all space2).

cbuffer HierCB : register(b0, space2)
{
    uint srcWidth;    // previous mip dimensions (for reduce clamp)
    uint srcHeight;
    uint dstWidth;    // this mip dimensions (dispatch range)
    uint dstHeight;
    uint writeSecond; // 0 = odd tail, produce mip N only
};

Texture2D<float>     g_Depth   : register(t0, space2);
// u0 serves both paths:
//   CSMain_Mip0:   writes (maxDepth, minDepth) into mip 0
//   CSMain_Reduce: reads the previous mip (writes go to u1/u2 below)
RWTexture2D<float2>  g_Mip     : register(u0, space2);
RWTexture2D<float2>  g_DstMipN : register(u1, space2);  // reduce path: mip N
RWTexture2D<float2>  g_DstMipM : register(u2, space2);  // reduce path: mip N+1

// .r = nearest (max under reverse-Z), .g = farthest (min).
float2 DPRCombine(float2 a, float2 b)
{
    return float2(max(a.r, b.r), min(a.g, b.g));
}

#define DPR_TYPE  float2
#define DPR_SRC   g_Mip
#define DPR_DST_A g_DstMipN
#define DPR_DST_B g_DstMipM
#include "depth_pyramid_reduce.hlsli"

// Mip 0: copy depth texel by texel, .r = .g = depth; max and min diverge from mip 1 on.
[numthreads(8, 8, 1)]
void CSMain_Mip0(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= dstWidth || dtid.y >= dstHeight) return;

    float d = g_Depth.Load(int3(dtid.xy, 0));
    g_Mip[dtid.xy] = float2(d, d);
}

// Mips N and N+1 from mip N-1. The odd-dimension remainder fold is in depth_pyramid_reduce.hlsli,
// shared with Hi-Z, which needs the identical rule.
[numthreads(8, 8, 1)]
void CSMain_Reduce(uint3 dtid : SV_DispatchThreadID,
                   uint3 gtid : SV_GroupThreadID,
                   uint3 gid  : SV_GroupID)
{
    DPR_Reduce2Mips(dtid, gtid, gid);
}
