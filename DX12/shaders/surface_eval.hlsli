#ifndef SURFACE_EVAL_HLSLI
#define SURFACE_EVAL_HLSLI

// One surface's response to one light, shared by the deferred pass (Lighting.ps), the
// forward path (custom_surface.hlsli) and water (water_common.hlsli). No register, no
// heap, no NPR: the toon per-light math is toon_eval.hlsli's, the NPR tail stays in
// Lighting.ps's NPR_PASS block. The includer provides material.hlsli's SHADING_MODEL_*
// defines.
#ifndef SHADING_MODEL_STANDARD
#error "surface_eval.hlsli needs material.hlsli's SHADING_MODEL_* - include it first"
#endif
#include "brdf.hlsli"
#include "lighting_types.hlsli"   // DirectLighting
#include "light_eval.hlsli"       // GPULight, SurfaceDistanceAttenuation, AngularAttenuation

// ==== SurfaceShading: the pixel's shading model, resolved once ====
// Every per-pixel quantity the non-Standard models need, decoded from RT4 and the
// material record before the clustered light loop, then handed to the sun path and to
// every punctual light by value.
//
// A struct, not loose arguments: the models need nine per-pixel values, and a positional
// list duplicated across call sites (the wave-uniform fast path and the divergent
// fallback) drifts apart. A new model is a field, not a signature change.
//
// Hoisted out of the loop on purpose: gMaterials[matIdx] is a structured-buffer load;
// inside the loop it would repeat per light per pixel, and inside the wave-uniform fast
// path it would be a divergent load next to the scalarised GPULight the fast path exists
// to produce.
struct SurfaceShading
{
    uint   model;         // SHADING_MODEL_*

    // Anisotropic: an orthonormal (T, B) pair in the shading normal's plane.
    float3 tangentWS;
    float3 bitangentWS;
    float  anisotropy;    // [-1,1]

    // Subsurface: tint already folded with strength and the per-pixel mask, so
    // `any(sssTint > 0)` is the single "is this model live" test, as for foliage. Never
    // partially applied downstream.
    float3 sssTint;
    float  sssOpacity;    // [0,1] 1 = solid
    float  sssWrap;       // [0,1] diffuse terminator wrap; 0 = off exactly

    // Two-sided foliage: transmission tint and wrap.
    float3 foliageTint;
    float  foliageWrap;

    // Clear coat: a second thin dielectric lobe over the base. Strength 0 makes both the lobe
    // and the base attenuation vanish exactly, so an unauthored clear-coat material is
    // bit-identical to Standard.
    float  clearCoat;
    float  clearCoatRoughness;
};

SurfaceShading InitSurfaceShading()
{
    SurfaceShading s;
    s.model       = SHADING_MODEL_STANDARD;
    s.tangentWS   = float3(1, 0, 0);
    s.bitangentWS = float3(0, 1, 0);
    s.anisotropy  = 0.0;
    s.sssTint     = float3(0, 0, 0);
    s.sssOpacity  = 1.0;
    s.sssWrap     = 0.0;
    s.foliageTint = float3(0, 0, 0);
    s.foliageWrap = 0.5;
    s.clearCoat   = 0.0;
    s.clearCoatRoughness = 0.0;
    return s;
}

bool SurfaceIsAnisotropic(SurfaceShading s) { return s.model == SHADING_MODEL_ANISOTROPIC; }
bool SurfaceIsClearCoat(SurfaceShading s)   { return s.model == SHADING_MODEL_CLEARCOAT; }
bool SurfaceHasTransmission(SurfaceShading s)
{
    return any(s.foliageTint > 0.0) || any(s.sssTint > 0.0);
}

// The reflected (surface) lobe for one light, routed by shading model. One
// call site shape for the sun and for every punctual light, so an anisotropic
// pixel cannot end up isotropic under a torch and directional under the sun.
void EvalSurfaceBRDF(SurfaceShading sm, float3 N, float3 V, float3 L, float3 F0,
                     float roughness, float metalness, float3 albedo,
                     out float3 outSpecular, out float3 outKD)
{
    [branch]
    if (SurfaceIsAnisotropic(sm))
        EvalCookTorranceAniso(N, V, L, sm.tangentWS, sm.bitangentWS, F0,
                              roughness, sm.anisotropy, metalness, albedo,
                              outSpecular, outKD);
    else
        EvalCookTorrance(N, V, L, F0, roughness, metalness, albedo,
                         outSpecular, outKD);

    // ---- Clear coat: a second lobe on top of whatever the base produced ----
    // Layered here, in the one function the sun path and every punctual light call, so a
    // coated surface cannot be coated under the sun and bare under a torch; the NPR spec
    // accumulators see the combined value, which a toon car-paint material wants.
    //
    // The base (diffuse and specular) is attenuated by the same Fresnel the coat lobe used:
    // energy the coat reflected never reached the base.
    [branch]
    if (sm.model == SHADING_MODEL_CLEARCOAT && sm.clearCoat > 0.0)
    {
        float coatFc;
        const float coat = ClearCoatLobe(N, V, L, sm.clearCoat,
                                         sm.clearCoatRoughness, coatFc);
        const float atten = 1.0 - coatFc;
        outSpecular = outSpecular * atten + coat;
        outKD       *= atten;
    }
}

// N.L for the diffuse lobe: subsurface wraps it past the terminator, every other model
// gets the plain saturate. Specular deliberately does not go through here: a wrapped
// highlight smears onto the shadow side.
float SurfaceDiffuseNoL(SurfaceShading sm, float rawNdotL)
{
    return (sm.model == SHADING_MODEL_SUBSURFACE)
         ? WrapDiffuseNoL(rawNdotL, sm.sssWrap)
         : saturate(rawNdotL);
}

// Everything transmitted through the surface for one light, unscaled by that light's
// radiance. Both models land in DirectLighting::transmission, so AccumulateDirect applies
// the light's shadow to them structurally.
float3 EvalSurfaceTransmission(SurfaceShading sm, float3 N, float3 V, float3 L, float ao)
{
    float3 t = float3(0, 0, 0);
    if (any(sm.foliageTint > 0.0))
        t += FoliageTransmission(N, V, L, sm.foliageTint, sm.foliageWrap);
    if (any(sm.sssTint > 0.0))
        t += SubsurfaceTransmission(N, V, L, sm.sssTint, sm.sssOpacity, ao);
    return t;
}


// ==== One punctual (clustered) light on one surface ====
// The part the deferred and forward loops share. The light's shadow lookup sits between
// the two functions in each caller: it is a binding (punctual_shadow), it must stay behind
// the early-outs below (outside the cone there is no SampleCmp to pay for), and whether a
// surface receives it is the caller's decision.
struct PunctualReach
{
    float3 L;           // unit, toward the light
    float  rawNdL;      // dot(N, L)
    float  NdL;         // saturate(rawNdL)
    float  atten;       // distance falloff (light_eval.hlsli)
    float  spotFactor;  // cone; 1.0 for a point light
    bool   transmitsOn; // foliage / subsurface transmission
    bool   wrapsOn;     // subsurface diffuse wraps past the terminator
};

// False when the light adds nothing to this pixel: off, out of range, outside
// its cone, or behind a surface that neither transmits nor wraps.
bool PunctualLightReach(GPULight light, float3 worldPos, float3 N, SurfaceShading sm,
                        out PunctualReach r)
{
    r = (PunctualReach)0;
    if (light.type == 0) return false;

    float3 Lp   = light.position - worldPos;
    float  dist = length(Lp);
    if (dist > light.radius) return false;

    r.L      = Lp / max(dist, 1e-6);
    r.rawNdL = dot(N, r.L);
    r.NdL    = saturate(r.rawNdL);
    // Two-sided foliage transmits light arriving from behind the leaf (NdL <= 0), so the
    // early-out must exempt it or a lantern behind a bush never shines through. Subsurface is
    // exempt too, and must be: an ear or a hand in front of a lamp is a back-hemisphere pixel,
    // and its wrapped diffuse survives past the terminator. Other materials still take the
    // early-out.
    r.transmitsOn = SurfaceHasTransmission(sm);
    r.wrapsOn     = (sm.model == SHADING_MODEL_SUBSURFACE) && (sm.sssWrap > 0.0);
    if (r.NdL <= 0.0 && !r.transmitsOn && !r.wrapsOn) return false;

    // Distance and cone stay separate (light_eval.hlsli's canonical punctual falloff) rather
    // than one EvalLocalLightAttenuation() call: that keeps the `spotFactor <= 0` early-out
    // below (skipping a shadow-map SampleCmp and a full Cook-Torrance) and the multiply order.
    r.atten      = SurfaceDistanceAttenuation(dist, light.radius);
    r.spotFactor = AngularAttenuation(r.L, light);
    // Outside the cone every term below is multiplied by zero, so bail before the shadow
    // matrix multiply / SampleCmp and Cook-Torrance. Inert for non-spot lights (spotFactor
    // stays 1.0).
    if (r.spotFactor <= 0.0) return false;
    return true;
}

// The surface's response to a light that reaches it, scaled by `radiance` (the
// light's colour x intensity x falloff, WITHOUT its shadow: visibility is the
// caller's, applied where VIS-LINEAR allows - AccumulateDirect in the deferred
// pass). specL / kDL are the bare BRDF terms the NPR tail rebuilds from.
// Returns false when only transmission reaches this pixel (the surface faces
// away and does not wrap); d then holds just that.
bool EvalPunctualSurface(PunctualReach r, float3 radiance, float3 N, float3 V, float3 F0,
                         float3 albedo, float roughness, float metalness, float ao,
                         SurfaceShading sm, out DirectLighting d,
                         out float3 specL, out float3 kDL)
{
    d     = ZeroDirect();
    specL = float3(0, 0, 0);
    kDL   = float3(0, 0, 0);

    // Back-lit foliage / subsurface: transmitted glow, added on top of (not instead of) the
    // reflected result. Computed before the NdL gate below because it lives entirely on the
    // far side, where NdL is 0.
    if (r.transmitsOn)
        d.transmission = EvalSurfaceTransmission(sm, N, V, r.L, ao) * radiance;

    // Everything below is the surface facing the light. A foliage pixel lit purely from
    // behind arrives with NdL == 0: skip the Cook-Torrance that would multiply out to zero, but
    // still accumulate, because the transmission term above is real.
    //
    // A wrapping subsurface pixel is the exception: its diffuse is non-zero past the
    // terminator by construction, so it must not take this shortcut.
    if (r.NdL <= 0.0 && !r.wrapsOn)
        return false;

    EvalSurfaceBRDF(sm, N, V, r.L, F0, roughness, metalness, albedo, specL, kDL);

    // Diffuse takes the model's N.L (wrapped for subsurface); specular keeps the true one, so
    // a highlight never survives past the terminator.
    const float diffNdL = SurfaceDiffuseNoL(sm, r.rawNdL);

    float3 diff = kDL * albedo / PI;
    d.diffuse  = diff  * radiance * diffNdL;
    d.specular = specL * radiance * r.NdL;
    return true;
}

#endif // SURFACE_EVAL_HLSLI
