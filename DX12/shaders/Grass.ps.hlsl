// Grass.ps.hlsl - deferred GBuffer writer for procedural grass blades.
//
// Writes the same 6 MRTs as Terrain.ps / GBuffer.ps, so grass takes part in
// deferred lighting (CSM shadows, DDGI, SSAO, SSR) with no special cases:
//   RT0 albedo, RT1 normal (encoded *0.5+0.5, a=0), RT2 surface
//   (roughness, metalness, AO, 0.5), RT3 velocity (NDC delta, unjittered),
//   RT4 extra, RT5 HDR emissive seed. Stencil ref 1 (PBR) is set by the pass.
//
// Albedo: root -> tip gradient x clump/noise tint, root AO darkening (vertex data
// from the MS). The MS owns two-sided orientation (it flips the per-vertex normal
// toward the camera BEFORE the up-blend): do not apply a second SV_IsFrontFace flip
// here, or back-facing pixels get inverted, downward GBuffer normals.

#include "Grass.hlsli"

struct PSIn
{
    float4 sv       : SV_Position;
    float3 wn       : NORMAL;
    float4 col      : COLOR;       // rgb = tint, a = AO along the blade
    float2 misc     : TEXCOORD0;   // x = t along blade
    float4 curClip  : TEXCOORD1;
    float4 prevClip : TEXCOORD2;
};

// The six MRTs: core/gbuffer_codec.hlsli's GBufferOut, through GBufferEncodeNoMaterial.
#include "core/gbuffer_codec.hlsli"

GBufferOut main(PSIn i)
{
    const float t = saturate(i.misc.x);
    float3 albedo = lerp(g_baseColor.rgb, g_tipColor.rgb, t) * i.col.rgb;

    float3 N = normalize(i.wn);

    // Raw NDC delta, as GBuffer.ps writes; the decoders (TAA / SSR / XeGTAO temporal)
    // apply the (0.5,-0.5) NDC -> UV scale themselves.
    float2 cur  = i.curClip.xy  / i.curClip.w;
    float2 prev = i.prevClip.xy / i.prevClip.w;

    // Grass is material 0: roughness from the CB, no metal, AO along the blade.
    return GBufferEncodeNoMaterial(albedo, N, g_roughness, 0.0, i.col.a, cur - prev);
}
