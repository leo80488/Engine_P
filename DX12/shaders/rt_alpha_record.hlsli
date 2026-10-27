#ifndef RT_ALPHA_RECORD_HLSLI
#define RT_ALPHA_RECORD_HLSLI
// RTAlphaRecord: per-TLAS-instance alpha-cutout descriptor, 40 bytes, indexed by
// InstanceID() like RTInstanceData but deliberately a separate compact table: the alpha
// test runs per traversal candidate, not per committed hit, and a 40-byte strided fetch
// beats a 128-byte one there. Do not widen RTInstanceData with these fields instead.
//
// The record has no UV transform: every candidate test that needs material UV tiling
// (HR_AlphaAcceptMip, HybridReflIntersectHW, RTShadow, RTAO) reads uvTilingOffset from
// the instance table itself, same InstanceID() index.
//
// CPU mirror: struct RTAlphaRecord in src/Renderer/DDGISceneAS.cpp (static_assert on
// sizeof and every offsetof); keep them in lockstep. Consumers declare their own binding:
//     StructuredBuffer<RTAlphaRecord> g_AlphaRecords : register(t32, space0);
struct RTAlphaRecord
{
    uint  flags;          //  0  RT_ALPHA_FLAG_*
    float alphaRef;       //  4  clip threshold (MaskClipValue, raster parity); opacity for RT_ALPHA_FLAG_BLEND
    int   albedoTexIdx;   //  8  BASECOLORMAP bindless index, -1 = none
    uint  vbBindlessIdx;  // 12  interleaved VB (or position stream)
    uint  ibBindlessIdx;  // 16
    uint  uvBufIdx;       // 20  de-interleaved float2 UV stream (kInvalid = none)
    uint  indexStart;     // 24  first index (count, not bytes)
    uint  uvStart;        // 28  base ELEMENT into the UV stream
    uint  vertexStride;   // 32  bytes per vertex in the interleaved VB
    uint  indexBytes;     // 36  2 = R16_UINT, else 4 = R32_UINT
};

// Material clips alpha: not a blend mode, IsAlphaTestEnabled() and alphaRef > 1/256
// (the gate in DDGISceneAS.cpp's fillMaterialProxy).
#define RT_ALPHA_FLAG_ALPHA_CLIP    1u
// MaterialComponent::IsDoubleSided(). A backface hit on such geometry is legitimate
// (foliage / hair / fence cards) and must be shaded, not rejected.
#define RT_ALPHA_FLAG_DOUBLE_SIDED  2u
// The instance uses de-interleaved streams (procedural primitives, skinned): UVs come
// from uvBufIdx/uvStart, not vbBindlessIdx + 24. RTInstanceData says this with
// `normalBufIdx != kInvalidBufferIndex`; the compact record has no normalBufIdx, so it
// is a flag bit.
#define RT_ALPHA_FLAG_DEINTERLEAVED 4u
// albedoTexIdx points at the material's opacity mask (Masked blend mode,
// MAT_TEX_OPACITYMAP): sample .r instead of .a. The record's texture index serves only
// the candidate alpha test, so repointing it never changes committed-hit shading (that
// reads the instance table's own albedoTexIdx).
//
// Limit: the record carries one texture, so with a mask RT tests mask.r only while
// raster clips on base.a x mask.r. They diverge only for materials with silhouette
// information in both textures; Unreal-style Masked content keeps base alpha opaque,
// where both paths agree. Author accordingly.
#define RT_ALPHA_FLAG_OPACITY_R     8u
// The instance is an alpha-blend (translucent) occluder - glass, water planes, smoke /
// fog cards - not a cutout. Exclusive with RT_ALPHA_FLAG_ALPHA_CLIP by construction
// (fillMaterialProxy sets blendShadow only for blend modes and alphaClip only for the
// others), so the two share `alphaRef`: a blend record's offset 4 holds the material's
// opacity (MaterialComponent::GetOpacity() == baseColor.w; 0 for Additive / Multiply).
//
// Do not treat a blend candidate like a cutout:
//   RTShadow   - accumulates transmittance *= (1 - alpha*opacity) and never commits,
//                so a 30%-opaque window darkens its shadow by 30%.
//   RTAO       - ignores the instance (glass must not occlude ambient).
//   HybridRefl - the main reflection ray commits it (a window is visible in a mirror);
//                its secondary ShadowVisibility ray passes through.
// The alpha sampled is BASECOLORMAP.a, never OPACITYMAP, as the forward transparent
// pass blends with.
#define RT_ALPHA_FLAG_BLEND         16u
// A Transmissive blend record: bits 8-31 of flags are the light one crossing keeps at full coverage,
// 8-bit unorm R, G, B (DDGISceneAS.cpp's TransmissiveShadowTransmittance per channel); the record's
// opacity stays the grey stand-in for every pass that reads one number. RTShadow multiplies the colour.
#define RT_ALPHA_FLAG_TINT          32u

float3 RTAlphaTint(RTAlphaRecord a)
{
    return float3((a.flags >> 8) & 0xFFu, (a.flags >> 16) & 0xFFu, (a.flags >> 24) & 0xFFu) / 255.0;
}

// Channel select for the RTShadow / RTAO / HybridRefl accept loops: samples the
// record's alpha texture once and returns .r (opacity mask) or .a (albedo alpha) per
// the flag.
float RTAlphaSampleAlpha(RTAlphaRecord a, Texture2D tex, SamplerState samp,
                         float2 uv, float mipLevel)
{
    const float4 t = tex.SampleLevel(samp, uv, mipLevel);
    return (a.flags & RT_ALPHA_FLAG_OPACITY_R) ? t.r : t.a;
}

#endif // RT_ALPHA_RECORD_HLSLI
