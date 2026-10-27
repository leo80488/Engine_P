// alpha_coverage.hlsli - the masked-surface coverage rule. ONE definition for every place
// that clips a masked surface, so the shadow a masked material casts is the silhouette
// it draws:
//   - custom_surface.hlsli's deferred GBuffer main (every opaque material),
//   - custom_surface.hlsli's SHADOW_HOOK main (a custom material's shadow),
//   - Shadow.ps.hlsl (the default material's shadow).
// Coverage `a` = the surface opacity (a hook's s.opacity; the default surface's
// baseColor.a x BASECOLORMAP.a) x the Masked-mode opacity mask. A Dithered material clips
// against a dither instead (temporal for the GBuffer, static for a shadow map; the caller
// picks). The includer declares g_AllTextures (bindless, t0 space2) and g_LinearWrap
// first. KEEP THIS FILE PURE ASCII.
#ifndef ALPHA_COVERAGE_HLSLI
#define ALPHA_COVERAGE_HLSLI

// The Masked-mode opacity mask (MAT_TEX_OPACITYMAP, .r) multiplies the
// coverage. Unbound (-1) leaves it alone, so plain alpha-test materials and
// hooks that author s.opacity themselves are unaffected.
float AC_ApplyOpacityMask(float a, int maskTexture, float2 uv)
{
    if (maskTexture >= 0)
        a *= g_AllTextures[maskTexture].Sample(g_LinearWrap, uv).r;
    return a;
}

// Ben Golus' anti-aliased alpha test: rescale the coverage so the cutoff sits
// at 0.5 with a one-pixel ramp (TAA jitter integrates it into a soft edge in
// the GBuffer; in a shadow map the ramp is one shadow texel). Clips below the
// cutoff and returns the ramp - the opacity the GBuffer keeps.
float AC_ClipGolus(float a, float alphaRef)
{
    float width  = max(fwidth(a), 1e-4);
    float scaled = (a - alphaRef) / width + 0.5;
    clip(scaled);
    return saturate(scaled);
}

#endif // ALPHA_COVERAGE_HLSLI
