// RTShadowResolve.cs.hlsl - decode the SIGMA output into the t45 sun mask.
//
// SIGMA stores sqrt(shadow) in OUT_SHADOW_TRANSLUCENCY; SIGMA_BackEnd_UnpackShadow
// squares it back. Result: 1 = fully lit, 0 = fully shadowed, the visibility
// semantic Lighting.ps min()s into the CSM result.
//
// The denoiser is SIGMA_SHADOW_TRANSLUCENCY, so the output is RGBA8 and this
// reads .y, not .x:
//   .x   = the binary shadow, still 0 for a pixel occluded only by glass
//          (reading it makes translucent objects cast solid shadows).
//   .yzw = the translucent shadow: the trace writes transmittance 1 where lit
//          and 0 where solidly blocked, so .yzw alone is the complete
//          visibility - per channel, grey but under Transmissive glass.
// The mask is RGBA8: .rgb that colour, .a its largest channel, the one number
// shadow.hlsli min()s into the CSM (RTShadowTint scales the sun by .rgb / .a).
// Both ride the same spatial + temporal filter because SIGMA's blur seeds the
// whole float4 SIGMA_TYPE from IN_TRANSLUCENCY when TRANSLUCENCY==1.
//
// Root signature map - must stay in sync with RTShadowPass.cpp:
//   [0]  ROOT_CBV b0  RTShadowConstants
//   [10] TABLE    t5  gShadowIn (SIGMA OUT_SHADOW_TRANSLUCENCY, RGBA8)
//   [11] TABLE    t6  gDepth    (reverse-Z, sky force-lit)
//   [12] TABLE    u4  OutFinal  (RGBA8 -> t45)

#include "rt_shadow_common.hlsli"

#define NRD_NORMAL_ENCODING    2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD.hlsli"

Texture2D<float4>  gShadowIn : register(t5, space0);
Texture2D<float>   gDepth    : register(t6, space0);
RWTexture2D<float4> OutFinal : register(u4, space0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= screenW || pixel.y >= screenH)
        return;

    // Reverse-Z sky = exactly 0 -> fully lit (no shadow ray was cast).
    if (gDepth.Load(int3(pixel, 0)) <= 0.0)
    {
        OutFinal[pixel] = 1.0;
        return;
    }

    const float4 st = SIGMA_BackEnd_UnpackShadow(gShadowIn.Load(int3(pixel, 0)));
    const float3 lit = saturate(st.yzw);   // translucent shadow, 1 = lit
    OutFinal[pixel] = float4(lit, max(lit.r, max(lit.g, lit.b)));
}
