#ifndef LIGHTING_TYPES_HLSLI
#define LIGHTING_TYPES_HLSLI

// lighting_types.hlsli - the direct/indirect x diffuse/specular taxonomy, as types
// rather than as discipline.
//
// The bug class it prevents: an NPR toon block that uses a sun visibility as a ramp
// texture coordinate (shadowArea = min(formTerm, SigmoidSharp(sunVisibility, ...)))
// and then multiplies the fetched ramp colour by the sun's radiance with no linear
// gate. The ramp's dark end is an authored non-black floor, so a fully shadowed
// pixel keeps an O(1) fraction of the sun. The invariant (VIS-LINEAR) is stated in
// full in Lighting.ps.hlsl; this header makes it checkable: AccumulateDirect is the
// only function that may read a physical visibility, and AccumulateIndirect cannot
// even be handed one.
//
// Production renderers do the same: UE returns FDirectLighting{Diffuse,Specular,
// Transmission} from every BxDF and passes FShadowTerms separately
// (`Shadow.SurfaceShadow` never occurs inside ShadingModels.ush), and HDRP names the
// same matrix (DirectLighting / IndirectLighting / AggregateLighting, with
// Accumulate* as the only mutators), as does DanbaidongRP, the toon reference this
// engine ports.
//
// ==== No resource access in this file ====
// No ResourceDescriptorHeap[], no Texture2D/StructuredBuffer, no register(), no
// cbuffer reference: pure POD and arithmetic. The taxonomy is data, and the passes
// that include it own their resources (shadow.hlsli reads the heap at three sites
// and is not the shared home for it). A heap read in a header a compute shader
// includes compiles fine and fails at PSO creation, with no line number, under a
// root signature without CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED. Keeping this file pure
// lets any graphics or compute shader include it without permutations. The check
// for the compute side is manual: compile tools/compute_include_probe.cs.hlsl and
// its DXIL must contain no createHandleFromHeap.

// ==== Visibility ====
// Two quantities that look alike and must never be conflated:
//
//   physical - the result of an occlusion query against real geometry: CSM PCF,
//              the per-object player map, the point/spot shadow maps, the RT sun
//              mask, the fitted bangs map. "Can this light reach this point?"
//              It is energy and may only ever multiply.
//
//   form     - authored shading that describes the surface, not occlusion: the
//              face SDF terminator, the sharpened half-lambert, the hair screen
//              mark. "How should this look?" It is a lookup key: it may index
//              ramps freely, but may never be the sole gate on energy - a form
//              term of 0 must not mean "no light", or a character's shadow side
//              goes black and stops receiving the torch standing next to it.
//
// Using a physical term as a form term is exactly the bug class above; the two
// fields make that mistake something you have to type out.
struct LightVisibility
{
    float physical;   // occlusion query result - multiply only
    float form;       // authored/artistic shaping - may index
};

LightVisibility MakeVisibility(float physical, float form)
{
    LightVisibility v;
    v.physical = physical;
    v.form     = form;
    return v;
}

LightVisibility FullyVisible()
{
    return MakeVisibility(1.0, 1.0);
}

// ==== The four buckets ====
// DIRECT   = proportional to one specific light's radiance (the sun, or a
//            clustered point/spot). Gated by that light's visibility.
// INDIRECT = IBL / DDGI / reflection probe / traced reflection / flat ambient.
//            Never gated by any light's shadow - a shadow says the sun cannot
//            reach here, not that the sky cannot.
struct DirectLighting
{
    float3 diffuse;
    float3 specular;
    float3 transmission;   // through-surface: two-sided foliage, SSS
};

struct IndirectLighting
{
    float3 diffuse;
    float3 specular;
};

struct AggregateLighting
{
    DirectLighting   direct;
    IndirectLighting indirect;
};

DirectLighting ZeroDirect()
{
    DirectLighting d;
    d.diffuse      = float3(0, 0, 0);
    d.specular     = float3(0, 0, 0);
    d.transmission = float3(0, 0, 0);
    return d;
}

IndirectLighting ZeroIndirect()
{
    IndirectLighting i;
    i.diffuse  = float3(0, 0, 0);
    i.specular = float3(0, 0, 0);
    return i;
}

AggregateLighting ZeroLighting()
{
    AggregateLighting a;
    a.direct   = ZeroDirect();
    a.indirect = ZeroIndirect();
    return a;
}

// ==== The only mutators ====
// Enforcement rule, the diff-checkable form of VIS-LINEAR:
//
//     Writes to LightVisibility::physical may occur only inside a function on
//     the producer list below. Reads of it may occur only inside
//     AccumulateDirect. Anywhere else is a violation.
//
//     Producer list (extend it here, in a visible diff, never silently):
//       shadow.hlsli   ComputeShadowFactor, ComputeShadowFactorExternalOnly,
//                      SampleCascadeShadow, SamplePerObjectShadow,
//                      SampleHairShadow, and the RT sun-mask fold
//       punctual_shadow.hlsli
//                      SampleSpotShadow, SamplePointShadow
//       RTDI.cs        SampleVisibility -> rtDiVisibility; multiplied once into RIS
//                      direct radiance before demodulation and denoising.
//
// tools/check_vis_linear.ps1 enforces the grep-able half of this.

// Applies a light's physical visibility to all three direct channels at once:
// the one multiply site for the whole engine.
void AccumulateDirect(inout AggregateLighting a, DirectLighting d, LightVisibility v)
{
    a.direct.diffuse      += d.diffuse      * v.physical;
    a.direct.specular     += d.specular     * v.physical;
    a.direct.transmission += d.transmission * v.physical;
}

// Takes no LightVisibility on purpose: the type-level guarantee that a light's
// shadow can never gate IBL / DDGI / probe / traced indirect. Do not add a
// visibility parameter; if you want one, the term you are adding is direct.
void AccumulateIndirect(inout AggregateLighting a, IndirectLighting i)
{
    a.indirect.diffuse  += i.diffuse;
    a.indirect.specular += i.specular;
}

// ==== Occlusion policy ====
// AO is an indirect visibility estimate: at full strength on direct light it
// double-counts (the shadow map already answered that) and crushes cavities.
// `directOcc` is the single named knob for how much AO touches direct light
// (DanbaidongRP's `lerp(1-_DirectOcclusion, 1, AO)`); let no other direct term see
// AO.
float3 PostEvaluate(AggregateLighting a, float indirectAO, float directOcc)
{
    return (a.direct.diffuse + a.direct.specular + a.direct.transmission) * directOcc
         + (a.indirect.diffuse + a.indirect.specular) * indirectAO;
}

// ==== Endpoint-pinned visibility sharpener ====
// The one sanctioned way to stylize a physical visibility: a toon cast-shadow
// terminator instead of a plain 16-tap PCF penumbra, without the leak, because it
// is endpoint-pinned:
//
//     lo = saturate(c - w) >= 0   =>   smoothstep(lo, hi, 0) == 0   exactly
//     hi = saturate(c + w) <= 1   =>   smoothstep(lo, hi, 1) == 1   exactly
//
// A fully occluded pixel contributes exactly zero direct energy at every parameter
// value - the property SigmoidSharp lacks:
//     SigmoidSharp(0, 0.5, 0.5)  = 1.778e-4
//     SigmoidSharp(0, 0.5, 0.1)  = 1.510e-1     <- 15% of the sun, in full shadow
//
// The result is still a multiplier, never a ramp coordinate. Tag call sites
// `// @vis-linear` with this proof.
//
//   v  physical visibility, 1 = lit
//   c  edge centre (0.5 = the PCF midpoint)
//   w  half-width. 0 = passthrough (bit-identical to saturate(v)).
float ToonVisibility(float v, float c, float w)
{
    v = saturate(v);
    if (w <= 0.0) return v;
    float lo = saturate(c - w);
    float hi = saturate(c + w);
    // Degenerate window (c-w and c+w both clamped to the same endpoint) would
    // make smoothstep divide by zero; fall back to a hard step, which is the
    // limit of the smoothstep as w -> 0 around c and is still endpoint-pinned.
    return (hi > lo) ? smoothstep(lo, hi, v) : step(c, v);
}

#endif // LIGHTING_TYPES_HLSLI
