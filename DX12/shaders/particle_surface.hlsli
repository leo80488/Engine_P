// particle_surface.hlsli - the engine-owned particle pixel template + author hook
// (EvaluateParticleSurface), the billboard-particle counterpart of custom_surface.hlsli. The
// weak default gives the radial-falloff + visual-mode + bindless-texture look. A custom
// particle PS replaces only the EvaluateParticleSurface hook and reuses this main():
//       #define PARTICLE_HAS_SURFACE
//       #include "particle_surface.hlsli"
//
//       ParticleSurface EvaluateParticleSurface(ParticleVaryings i)
//       {
//           ParticleSurface s;
//           s.color = i.color.rgb;     // i.uv / i.lifeT / i.texIdx available
//           s.alpha = i.color.a;
//           return s;
//       }

#ifndef PARTICLE_SURFACE_HLSLI
#define PARTICLE_SURFACE_HLSLI

#include "particle_common.hlsli"
#include "debug_shade.hlsli"   // nothing outside the DEBUG_SHADE permutation
#include "core/color.hlsli"   // Luminance

// The bindless texture table at t0 space2 of the graphics root sig (shared with
// GBufferPass / TransparentPass).
Texture2D<float4> g_AllTextures[] : register(t0, space2);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define gLinear SmpFixed(SMP_LINEAR_WRAP)

// Author output. The engine main() composites color + alpha and discards near-transparent
// pixels.
struct ParticleSurface
{
    float3 color;   // RGB (emissive / albedo)
    float  alpha;   // coverage 0..1
};

// Author-provided hook (declared for main(); defined by the author file or the weak
// default below).
ParticleSurface EvaluateParticleSurface(ParticleVaryings i);

#ifndef PARTICLE_HAS_SURFACE

// ---- Color palettes (weak-default helpers) ----
float3 PS_FirePalette(float t)
{
    float3 stops[5] = {
        float3(0.03, 0.02, 0.01),  // dying ember
        float3(1.0,  0.15, 0.02),
        float3(1.0,  0.55, 0.10),
        float3(1.0,  0.88, 0.40),
        float3(1.0,  1.0,  0.95)   // hottest
    };
    float f = saturate(1.0 - t) * 4.0;
    int i = (int)floor(f);
    float blend = f - float(i);
    int hi = min(i + 1, 4);
    return lerp(stops[i], stops[hi], blend);
}

// Cheap value noise for electric flicker (no texture required).
#include "core/hash.hlsli"

// Weak default: soft circle, visual-mode palette, optional bindless texture.
ParticleSurface EvaluateParticleSurface(ParticleVaryings i)
{
    ParticleSurface s;

    // Radial distance from quad centre (0 at centre, 1 at corners).
    float2 d = i.uv - 0.5;
    float  r = saturate(length(d) * 2.0);

    // Baseline soft-circle mask.
    float alpha = 1.0 - smoothstep(0.5, 1.0, r);

    float3 rgb = i.color.rgb;

    switch (i.visMode)
    {
    case PARTICLE_VISUAL_FIRE:
    {
        rgb   = PS_FirePalette(i.lifeT);
        alpha = pow(alpha, 0.6);
        break;
    }
    case PARTICLE_VISUAL_SMOKE:
    {
        float luma = Luminance(rgb);
        rgb   = lerp(rgb, float3(luma, luma, luma), 0.6);
        alpha *= alpha;
        break;
    }
    case PARTICLE_VISUAL_ELECTRIC:
    {
        float flicker = 0.7 + 0.3 * HashHoskins12(i.uv * 17.0 + i.lifeT * 31.0);
        rgb   = lerp(rgb, float3(0.6, 0.8, 1.8), 0.7) * flicker * 1.6;
        alpha = smoothstep(0.45, 0.3, r);
        break;
    }
    default: // PARTICLE_VISUAL_FLAT
        break;
    }

    // Texture modulation (bindless), applied on top of the visual-mode output.
    if (i.texIdx != 0xFFFFFFFFu)
    {
        float4 tex = g_AllTextures[NonUniformResourceIndex(i.texIdx)]
                         .SampleLevel(gLinear, i.uv, 0);
        rgb   *= tex.rgb;
        alpha *= tex.a;
    }

    s.color = rgb;
    s.alpha = i.color.a * alpha;
    return s;
}

#endif // PARTICLE_HAS_SURFACE

// ==== Composite ====
// Particles draw after the fog composites, so they get the fog the opaque scene got at their
// own depth, and fade out where they meet the opaque surface behind them instead of cutting a
// hard line into it. Both depend on the bucket's blend state, which only the root constant
// tells the PS (g_bucketBlend, particle_common.hlsli).
#include "fog_apply.hlsli"   // FogApplyCB b4 + s3, bound by ParticleRenderPass

float4 PS_Composite(float4 c, float4 svPos)
{
    const float     fade = Fog_SoftFade(svPos);
    const FogSample f    = Fog_Evaluate(svPos);
    [branch] switch (g_bucketBlend)
    {
    case 1u:   // Additive (SRC_ALPHA, ONE): the blend weighs rgb by alpha
        c.a *= fade;
        return Fog_ApplyAdditive(c, f);
    case 2u:   // Premultiplied (ONE, INV_SRC_ALPHA)
        c *= fade;
        return Fog_ApplyPremultiplied(c, f);
    case 3u:   // Multiply (DEST_COLOR, INV_SRC_ALPHA): fading both terms -> factor 1
        c *= fade;
        return Fog_ApplyMultiply(c, f);
    default:   // Alpha (SRC_ALPHA, INV_SRC_ALPHA)
        c.a *= fade;
        return Fog_ApplyAlpha(c, f);
    }
}

// ==== Main (engine-owned) ====
#if PARTICLE_LIT

// Forward-lit particles (diffuse: sun + ambient), for soft, slow particles (smoke, dust,
// steam): s.color is albedo, shaded by the directional sun and the environment ambient with
// the spherical billboard normal. Diffuse-only on purpose (smoke is not specular); no
// roughness/metallic/probes. Per-emitter opt-in; ParticleRenderPass binds this only on lit
// buckets.
#define LIGHT_CB_REGISTER b2
#include "light_cb.hlsli"   // lightDir/lightColor, the IBL fields, ambientColor/ambientIntensity
#include "sky_sh.hlsli"     // gSkySH (t19) + EvalSH2

TextureCube  gIrradiance : register(t6, space0);
#define gIBLSampler SmpFixed(SMP_LINEAR_WRAP)

#if PARTICLE_SHADOW
// Per-emitter sun shadow - a second opt-in on top of PARTICLE_LIT
// (ParticleBucket::receiveShadow), not implied by it. Smoke, dust and steam should go dark
// under a canopy; fire, sparks and magic look wrong when a shadow edge crosses them, so the
// emitter decides.
// Confined to this permutation so other emitters pay nothing (no extra binding, sample or
// PSO). No root-signature cost: Gfx::ShadowSRV (t9..t11) and s2 exist in the shared graphics
// signature; the pass binds them for this bucket.
// PER_OBJECT_SHADOW is on for the same reason as the mesh forward path: while the high-res
// per-object map is active, ShadowPass excludes the character from the cascades, so cascades
// alone would let a character stand in a dust cloud and cast nothing into it.
Texture2DArray<float>  gShadowCascades : register(t9, space0);
#define gShadowSampler SmpCmp(SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE)
#define PER_OBJECT_SHADOW
#include "shadow.hlsli"
#endif

float4 main(ParticleVaryings i) : SV_TARGET
{
    DEBUG_SHADE_INVOCATION(i.pos);   // overdraw
    ParticleSurface s = EvaluateParticleSurface(i);
    if (s.alpha < 0.004) discard;

    float3 N      = normalize(i.normalWS);
    float3 albedo = s.color;

    // Direct sun (Lambert diffuse). lightDir is FROM the light, so L = -lightDir.
    // The 1/PI is the Lambert normalisation every other surface carries (Lighting.ps.hlsl and
    // custom_surface.hlsli both divide by PI); do not drop it, or lit particles come out PI x
    // brighter than an opaque surface of the same albedo under the same sun.
    float3 L      = normalize(-lightDir);
    float  NdotL  = saturate(dot(N, L));
#if PARTICLE_SHADOW
    // @vis-linear — plain multiplier on the DIRECT term only. Never on `ambient`
    // below: the sun being occluded says nothing about the sky.
    const float sunVis = ComputeShadowFactor(i.worldPos, N, i.pos.xy);
#else
    const float sunVis = 1.0;
#endif
    // 1/PI spelled out: nothing on the particle path includes core/math.hlsli (PI).
    const float kInvPi = 0.31830988618;
    float3 direct = albedo * kInvPi * lightColor * NdotL * sunVis;

    // Ambient diffuse. Sky IBL (SH or irradiance cubemap) when iblRadianceMips > 0: the sky
    // modes and FlatColor's uniform sky cube; in None, iblRadianceMips is 0 and the flat fill
    // below carries it. Renderer_IBL.cpp zeroes ambientColor whenever the IBL carries the
    // ambient, so at most one contributes. The mesh gates also admit DDGI / probes / SSGI, which
    // particles do not sample, so here the gate covers the sky term alone. iblRadianceMips is a
    // LightCB field: the branch is uniform and Sample's implicit gradients stay defined.
    float3 ambient = float3(0, 0, 0);
    [branch] if (iblRadianceMips > 0)
    {
        const float3 ambIrr = (iblUseSH != 0) ? EvalSH2(N)
                                              : gIrradiance.Sample(gIBLSampler, N).rgb;
        ambient = albedo * ambIrr * iblStrength;
    }
    // Flat fill ambient (WorldEnvironment None, or FlatColor when its uniform sky cube could not
    // be created): the constant Lighting.ps.hlsl and custom_surface.hlsli add to every mesh, zero
    // otherwise. The meshes' `* ao` has no particle counterpart.
    ambient += albedo * ambientColor * ambientIntensity;

    // i.intensity carries the emissiveFade taper -> keep it as an overall scale so lit
    // particles still fade over life.
    return PS_Composite(float4((direct + ambient) * i.intensity, s.alpha), i.pos);
}

#else // unlit (emissive composite)

float4 main(ParticleVaryings i) : SV_TARGET
{
    DEBUG_SHADE_INVOCATION(i.pos);   // overdraw
    ParticleSurface s = EvaluateParticleSurface(i);
    if (s.alpha < 0.004) discard;
    // Emissive brightness multiplier, applied after the surface hook so it scales every visual
    // mode and every custom particle PS. HDR: values > 1 push the output past the bloom
    // threshold.
    return PS_Composite(float4(s.color * i.intensity, s.alpha), i.pos);
}

#endif // PARTICLE_LIT

#endif // PARTICLE_SURFACE_HLSLI
