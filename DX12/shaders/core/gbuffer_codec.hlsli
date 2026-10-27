// core/gbuffer_codec.hlsli - the GBuffer's packed fields, encoded and decoded in
// one place.
//
// Rules for every file in shaders/core:
//   * pure arithmetic: no register(), no ResourceDescriptorHeap, so any shader
//     under any root signature can include it;
//   * pure ASCII: the offline SDK dxc reads the file in the system code page;
//   * no token pasting and no permutation define name: gen_perm_relevance.py
//     would widen the permutation mask of every shader that includes it;
//   * valid HLSL 2018 and 2021 (some shaders compile with -HV 2021): no vector
//     condition in a ?: (2021 wants select()) - write per-component scalars.

#ifndef CORE_GBUFFER_CODEC_HLSLI
#define CORE_GBUFFER_CODEC_HLSLI

// ==== RT1.a: the material index and the XeGTAO-exclude flag ====
// RT1 is R16G16B16A16_UNORM, so alpha is an exact 16-bit integer: the material
// index in bits 0..14 (4096 materials need 12), bit 15 = excluded from XeGTAO
// (MAT_FLAG_EXCLUDE_FROM_SSAO). The *65535+0.5 round trip is exact: UNORM
// quantizes to raw/65535, whose float32 error (~1e-7) is four orders below the
// 1/65535 step. Read it with a texel-exact load (a 1:1 sample at texel centres
// is the same); a filtered tap blends two indices into a third. Do not move it to
// an FP16 format: its 11-bit significand holds integers exactly only up to 2048.
static const uint GBUFFER_MATIDX_MASK    = 0x7FFFu;
static const uint GBUFFER_MATIDX_NO_SSAO = 0x8000u;

// The 16 stored bits.
uint GBufferMatIdxRaw(float a)
{
    return (uint)(a * 65535.0 + 0.5);
}

uint GBufferDecodeMatIdx(float a)
{
    return GBufferMatIdxRaw(a) & GBUFFER_MATIDX_MASK;
}

bool GBufferMatIdxNoSsao(float a)
{
    return (GBufferMatIdxRaw(a) & GBUFFER_MATIDX_NO_SSAO) != 0u;
}

float GBufferEncodeMatIdx(uint matIdx, bool excludeSsao)
{
    const uint raw = (matIdx & GBUFFER_MATIDX_MASK) | (excludeSsao ? GBUFFER_MATIDX_NO_SSAO : 0u);
    return float(raw) / 65535.0;
}

// ==== The six MRTs ====
// The engine GBuffer schema: do not change it without updating Lighting.ps and
// the XeGTAO decoders (GBuffer.ps.hlsl's header). Written by material_surface's
// MS_EncodeGBuffer for every material, and by GBufferEncodeNoMaterial below.
struct GBufferOut
{
    float4 albedo   : SV_TARGET0;   // baseColor (no emissive)
    float4 normal   : SV_TARGET1;   // worldNormal*0.5+0.5, .a = 16-bit packed matIdx
    float4 surface  : SV_TARGET2;   // roughness, metalness, ao, reflectance
    float2 velocity : SV_TARGET3;   // NDC motion vector
    float4 extra    : SV_TARGET4;   // shading-model scratch
    float4 sceneCol : SV_TARGET5;   // HdrSceneColor: emissive seed (additive lighting)
};

// A surface with no material-table entry: terrain and grass. They draw under
// their own root signatures (Terrain.ps binds its heightmap at t2 space0, the
// slot every material pass gives the material table), so MS_EncodeGBuffer,
// which reads that table, is out of their reach. They are material 0 (the
// deferred pass looks its model up like any pixel's), not excluded from XeGTAO,
// RT4 zero, reflectance 0.5 (F0 0.04), no emissive. normalWS is already unit
// length, and nothing is saturated here.
GBufferOut GBufferEncodeNoMaterial(float3 baseColor, float3 normalWS, float roughness,
                                   float metallic, float ao, float2 velocity)
{
    GBufferOut o;
    o.albedo   = float4(baseColor, 1.0);
    o.normal   = float4(normalWS * 0.5 + 0.5, GBufferEncodeMatIdx(0u, false));
    o.surface  = float4(roughness, metallic, ao, 0.5);
    o.velocity = velocity;
    o.extra    = float4(0.0, 0.0, 0.0, 0.0);
    o.sceneCol = float4(0.0, 0.0, 0.0, 1.0);
    return o;
}

#endif // CORE_GBUFFER_CODEC_HLSLI
