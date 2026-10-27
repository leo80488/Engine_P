#ifndef INDIRECT_SPECULAR_HLSLI
#define INDIRECT_SPECULAR_HLSLI

// Shared contracts for every indirect-specular producer/consumer.
//
// All reflection sources return pre-Fresnel radiance plus coverage. Sources are
// composed in radiance space and the split-sum EnvBRDF is applied exactly once at
// the final surface. Keep the ray-class values stable and in this order: they fill
// the low two bits of SSRTrace's R8_UINT rayClass (the high bits stay reserved for
// the RT continuation), and SSRResolve treats a class <= SSR_HIT_SKY as a hit.
// TODO(indirect_specular_refactor_design Phase 6): an RT continuation that resumes
// SSR_MISS_EDGE / SSR_MISS_THICKNESS rays from their last valid screen position.
#define SSR_HIT_GEOMETRY   0u
#define SSR_HIT_SKY        1u
#define SSR_MISS_EDGE      2u
#define SSR_MISS_THICKNESS 3u

static const float INDIRECT_SPECULAR_DDGI_ROUGH_START = 0.4;
static const float INDIRECT_SPECULAR_DDGI_ROUGH_END   = 0.7;
static const float INDIRECT_SPECULAR_HORIZON_SCALE    = 1.1;

float IndirectSpecular_RoughFade(float roughness, float fadeStart, float fadeEnd)
{
    return 1.0 - smoothstep(fadeStart, max(fadeEnd, fadeStart + 1e-4), roughness);
}

float IndirectSpecular_HorizonVisibility(float3 reflectionDir)
{
    return saturate(1.0 + INDIRECT_SPECULAR_HORIZON_SCALE * reflectionDir.y);
}

// Lagarde-style specular occlusion. Fallback-only: traced SSR/RTR radiance
// already contains visibility and must not be darkened twice.
float IndirectSpecular_SpecularOcclusion(float ao, float NoV, float roughness)
{
    const float exponent = exp2(-16.0 * roughness - 1.0);
    return saturate(pow(saturate(NoV + ao), exponent) - 1.0 + ao);
}

float3 IndirectSpecular_ApplyEnvBRDF(float3 radiance, float3 F0, float2 envBRDF)
{
    return radiance * (F0 * envBRDF.x + envBRDF.y);
}

// Pre-integrated split-sum BRDF LUT lookup (asset/IBL/BRDF_LUT.itex, RG16F).
// Use this, not a bare Sample(); it fixes two edge hazards that ordinary material
// settings hit:
//  1) Wrap addressing. The LUT is indexed (u = NoV, v = perceptual roughness) over
//     the closed range [0,1], so roughness == 1 (e.g. Roughness Min == Max == 1)
//     and NoV == 1 reach the texture edge. The passes bind it with the IBL sampler,
//     trilinear-WRAP for the cubemaps (LightingPass.cpp / TransparentPass.cpp), so
//     v = 1.0 would blend the last row 50/50 with row 0 and a fully rough surface
//     would take half its envBRDF from the smooth end. Clamping the coordinate is
//     correct whatever sampler a pass binds at s1.
//  2) Implicit mip selection. The LUT has a full mip chain; Sample() picks a mip
//     from the derivatives of (NoV, roughness), blurring the table on curved or
//     normal-mapped surfaces. SampleLevel(0) pins mip 0.
// GetDimensions keeps the half-texel inset correct at any LUT resolution; one
// resinfo is noise next to the loops this sits in.
float2 IndirectSpecular_SampleEnvBRDF(Texture2D<float2> lut, SamplerState samp,
                                      float NoV, float roughness)
{
    float w, h;
    lut.GetDimensions(w, h);
    const float2 inset = 0.5 / max(float2(w, h), float2(1.0, 1.0));
    const float2 uv    = clamp(float2(NoV, roughness), inset, 1.0 - inset);
    return lut.SampleLevel(samp, uv, 0.0);
}

// Probe cubemaps store conditional geometry radiance in rgb and geometry
// coverage in alpha. Accumulation stays premultiplied so overlapping probes
// remain a deterministic top-down fall-through chain and never parallax-warp
// the sky captured between geometry silhouettes.
struct IndirectSpecularProbeAccum
{
    float3 premultipliedRadiance;
    float  coverage;
};

IndirectSpecularProbeAccum IndirectSpecular_MakeProbeAccum()
{
    IndirectSpecularProbeAccum result;
    result.premultipliedRadiance = float3(0.0, 0.0, 0.0);
    result.coverage = 0.0;
    return result;
}

void IndirectSpecular_AddProbe(inout IndirectSpecularProbeAccum accum,
                               float4 probeSample,
                               float volumeWeight,
                               float intensity)
{
    const float available = saturate(1.0 - accum.coverage);
    const float layerCoverage = saturate(volumeWeight)
                              * saturate(probeSample.a)
                              * available;
    accum.premultipliedRadiance += probeSample.rgb * intensity * layerCoverage;
    accum.coverage += layerCoverage;
}

float3 IndirectSpecular_ResolveProbeFallback(IndirectSpecularProbeAccum probes,
                                             float3 distantFallback)
{
    return probes.premultipliedRadiance
         + distantFallback * saturate(1.0 - probes.coverage);
}

#endif // INDIRECT_SPECULAR_HLSLI
