// HiZReduce.cs.hlsl - generates Hi-Z mip N (and, when writeSecond != 0, mip N+1) from
// mip N-1.
//
// The source mip is read through a UAV so the whole Hi-Z texture stays in
// UNORDERED_ACCESS across every reduce dispatch (UAV barriers between dispatches
// serialize the writes). That avoids per-subresource SRV/UAV transitions, which the
// engine's per-texture state tracker cannot express without desync. HiZGenerate_CS
// produces mip 0 from the depth SRV.
//
// The reduction lives in depth_pyramid_reduce.hlsli, shared with the SSR depth hierarchy:
// only the payload (float vs float2) and the combine operator differ, both selected by
// define. That file explains the two mips per dispatch and the odd-dimension fold that
// keeps the non-power-of-two pyramid conservative.
//
// Root sig (shared compute layout):
//   [0]  CBV b0 space2  - HiZCB (mip N-1 dims, mip N dims, writeSecond)
//   [4]  UAV u0 space2  - source mip N-1 (read-only access)
//   [5]  UAV u1 space2  - destination mip N
//  [14]  UAV u2 space2  - destination mip N+1 (bound to mip N on the odd tail,
//                         where writeSecond == 0 keeps it unwritten)

cbuffer HiZCB : register(b0, space2)
{
    uint srcWidth;    // mip N-1 dimensions
    uint srcHeight;
    uint dstWidth;    // mip N dimensions (dispatch range)
    uint dstHeight;
    uint writeSecond; // 0 = odd tail, produce mip N only
};

RWTexture2D<float> g_SrcMip  : register(u0, space2);
RWTexture2D<float> g_DstMip  : register(u1, space2);
RWTexture2D<float> g_DstMip2 : register(u2, space2);

// Reverse-Z: near=1 far=0, min = farthest occluder in the tile.
float DPRCombine(float a, float b) { return min(a, b); }

#define DPR_TYPE  float
#define DPR_SRC   g_SrcMip
#define DPR_DST_A g_DstMip
#define DPR_DST_B g_DstMip2
#include "depth_pyramid_reduce.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID,
            uint3 gtid : SV_GroupThreadID,
            uint3 gid  : SV_GroupID)
{
    DPR_Reduce2Mips(dtid, gtid, gid);
}
