#ifndef MATERIAL_AUTHORING_HLSLI
#define MATERIAL_AUTHORING_HLSLI

// material_authoring.hlsli - the shared helper set every custom material shader should use
// (the endfield EF_* helpers in asset/Shaders/endfield/endfield_common.hlsli, generalised).
// Include it after custom_material.hlsli: it needs g_Materials and the MaterialPixelParams /
// SurfaceData types. Each helper guards a hazard that fails silently (no compile error, no
// debug-layer message; the material just looks broken).

// ==== An unassigned space3 slot samples white unless it says otherwise ====
// The renderer pre-fills every unclaimed custom-texture slot with a default-white SRV, so an
// empty slot returns (1,1,1,1):
//     empty emissive map  -> the whole mesh glows
//     empty PBR/ARM map   -> metallic 1, smoothness 1  (a chrome mirror)
//     empty mask map      -> mask = 1 everywhere, i.e. the effect always on
// A declaration names what its empty slot samples:
//     Texture2D _Mask : register(t2, space3);   //@default black
// (white | black | grey | normal; `@srgb` makes grey sRGB mid-grey). For a per-call
// fallback, the per-slot bindless id is visible to the shader and is -1 when unbound (the
// test below). Slot index == the space3 register index (t0 -> 0, t1 -> 1, ...).
bool MA_HasTex(uint matIdx, uint slot)
{
    return g_Materials[matIdx].customTextureIds[slot] >= 0;
}

// Samples a custom texture, or returns fallback when slot is unbound. Slots bind by register
// (Renderer::PackCustomMaterialBindings), so a texture DXC strips has no Inspector row and no
// binding in that variant without shifting the others. Samples unconditionally and selects
// the result: one sample of a 1x1 texture when the slot is empty.
float4 MA_Tex(Texture2D t, SamplerState s, float2 uv,
              uint matIdx, uint slot, float4 fallback)
{
    float4 v = t.Sample(s, uv);
    return MA_HasTex(matIdx, slot) ? v : fallback;
}

float4 MA_TexLevel(Texture2D t, SamplerState s, float2 uv,
                   uint matIdx, uint slot, float4 fallback, float lod)
{
    float4 v = t.SampleLevel(s, uv, lod);
    return MA_HasTex(matIdx, slot) ? v : fallback;
}

// ==== normalize(0) is NaN, and one NaN pixel poisons TAA for seconds ====
// Degenerate tangents, zero-length interpolated normals and zero-length flow vectors all
// happen in real content. A NaN written to the HDR target survives into the TAA history and
// smears until the history is reset.
float3 MA_SafeNormalize(float3 v) { return v / max(1e-5, length(v)); }
float2 MA_SafeNormalize2(float2 v) { return v / max(1e-5, length(v)); }

// Two-channel (BC5) tangent-space normal -> world, with a strength dial that
// lerps against the geometric normal. `strength` 0 = flat, 1 = full map.
// tangentToWorld rows are (T, BT, N), which is what MaterialPixelParams supplies.
float3 MA_DecodeNormal(float2 rg, float strength, float3x3 tangentToWorld)
{
    const float2 v  = rg * 2.0 - 1.0;
    const float  z  = max(sqrt(saturate(1.0 - dot(v, v))), 1e-16);
    const float3 Nm = normalize(mul(float3(v, z), tangentToWorld));
    const float3 Ng = tangentToWorld[2];
    return normalize(lerp(Ng, Nm, max(strength, 0.0)));
}

#endif // MATERIAL_AUTHORING_HLSLI
