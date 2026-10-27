// Bindless sprite-sheet billboard PS. Samples the atlas page at the CPU-baked sub-rect UV and
// modulates by the per-vertex HDR colour (tint * emissive, alpha = tint.a * opacity);
// texIdx == 0xFFFFFFFF -> untextured, the colour alone. Straight alpha: the PSO's source
// factor is SRC_ALPHA for both blends.
// Billboards draw after the fog composite, so they get the opaque scene's fog at their own
// depth and, when they depth-test, soft-fade where the quad meets the surface behind it. Both
// depend on PSO state the PS cannot see: BillboardFXPass passes it per draw in the b0 flags.

#include "fog_apply.hlsli"   // FogApplyCB b4 + s3, bound by BillboardFXPass

Texture2D    g_BindlessTex[] : register(t0, space2);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_Smp SmpFixed(SMP_LINEAR_CLAMP)

// Must match BillboardFXPass kDrawFlag*.
#define BILLBOARD_FLAG_ADDITIVE   1u   // dest ONE (else INV_SRC_ALPHA)
#define BILLBOARD_FLAG_DEPTHTEST  2u   // the quad depth-tests: soft-fade it

cbuffer BillboardDrawCB : register(b0, space0)
{
    uint  g_billboardFlags;
    uint3 _billboardPad;
};

struct PSIn
{
    float4 pos                  : SV_POSITION;
    float2 uv                   : TEXCOORD0;
    nointerpolation uint texIdx : TEXCOORD1;
    float4 color                : COLOR0;
};

float4 main(PSIn i) : SV_TARGET
{
    float4 c = i.color;                       // untextured: flat colour
    if (i.texIdx != 0xFFFFFFFFu)
        c *= g_BindlessTex[NonUniformResourceIndex(i.texIdx)].Sample(g_Smp, i.uv);   // tex x (tint * emissive, opacity)

    // A quad that ignores depth (always on top) has no surface to fade against.
    if (g_billboardFlags & BILLBOARD_FLAG_DEPTHTEST)
        c.a *= Fog_SoftFade(i.pos);

    const FogSample f = Fog_Evaluate(i.pos);
    return (g_billboardFlags & BILLBOARD_FLAG_ADDITIVE) ? Fog_ApplyAdditive(c, f)   // SRC_ALPHA / ONE
                                                        : Fog_ApplyAlpha(c, f);     // SRC_ALPHA / INV_SRC_ALPHA
}
