#include "spot_shadow_view.hlsli"
// Lighting.ps.hlsl - the deferred lighting pass: PBR, NPR (toon) and Unlit, with clustered
// punctual lights.
//
// Both MRT outputs blend additively onto targets the GBuffer pass pre-seeded with material
// emissive (LightingPass.cpp configures the blend):
//   SV_Target0 = lighting minus the traced-specular and SSGI deltas (the reflection source)
//   SV_Target1 = final lighting
// Target0 feeds the next frame's screen-radiance pyramid, so the final HDR reflection never
// reflects itself across frames.
//
// Three permutations, each stencil-gated to its own pixels (PBR = 1, NPR = 2, Unlit = 3):
//   NPR_PASS=0, UNLIT=0: PBR Cook-Torrance.
//   NPR_PASS=1:          the toon model: the material's ramp texture - or, without one, its two
//                        ramp colours - in place of N.L (the toon block near the end of main).
//   UNLIT=1:             baseColor only.

#include "material.hlsli"
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx
#include "core/color.hlsli"   // Luminance
#include "material_types.hlsli"   // DIRECT_ADDITIVE_ENCODE_SCALE (the RT4.a packing)
#include "lighting_types.hlsli"   // LightVisibility / DirectLighting / IndirectLighting /
                                  // AggregateLighting + ToonVisibility: POD and
                                  // arithmetic only (the header says why).
#include "shading_models.hlsli"    // RT4 codec for Anisotropic / Subsurface, shared
                                  // with its writer, MS_EncodeGBuffer (GBuffer
                                  // pass): the pair must never drift.
#include "shading_model_table.hlsli" // which models ReSTIR DI lights (the fold below)
#include "brdf.hlsli"
#include "indirect_specular.hlsli"
#include "sky_sh.hlsli"            // gSkySH SRV + EvalSH2
#include "reflection_probe.hlsli"  // ReflectionProbe + ComputeProbeWeight / SampleProbe.
                                   // Leave REFL_PROBE_BUFFER_REGISTER undefined: that
                                   // selects the header's bindless path, which opens the
                                   // probe buffer from LightCB.reflProbeBufIdx inside
                                   // the `reflectionProbeCount > 0` gate.

// LightCB on b1: this fullscreen draw has no PerViewCB.
#define LIGHT_CB_REGISTER b1
#include "light_cb.hlsli"
#include "cloud_shadow.hlsli"       // SampleCloudShadow: cloud shadow through LightCB's
                                    // bindless index (HEAP_DIRECTLY_INDEXED root sig)
#include "view_mode_common.hlsli"   // VIEW_MODE_* + WIREFRAME_COLOR (LightCB.viewMode)
#include "face_sdf.hlsli"           // SHARED with the face GBuffer PS: RT4 encoding,
                                    // marker versions, per-light terminator maths

// GBuffer inputs
Texture2D<float4> gAlbedo   : register(t2, space0);
Texture2D<float4> gNormal   : register(t3, space0);
Texture2D<float4> gSurface  : register(t4, space0);
Texture2D<float>  gDepth    : register(t5, space0);

// IBL
TextureCube       gIrradiance : register(t6, space0);
TextureCube       gRadiance   : register(t7, space0);
Texture2D<float2> gBRDFLUT    : register(t8, space0);

// Per-cluster probe list, fed by ClusterCullProbes.cs.hlsl. Lighting-only (TransparentPass
// iterates every probe without the cluster lookup), so it stays here, not in reflection_probe.hlsli.
// Both buffers are bindless (LightCB.reflProbeGridIdx / reflProbeIndexIdx), opened inside the
// `reflectionProbeCount > 0` block. t16 and t23..t45 are out of the root signature for good
// (RootSignatureLayout.h): declaring one again fails ShaderBindingValidation at startup. The
// struct is the element type the null SRV's StructureByteStride (8) is matched against.
struct ProbeGridEntry
{
    uint offset;
    uint count;
};

// No SSR-result SRV: the traced signal arrives via rtReflectionIdx (the standalone SSR chain's
// upsample output in the FullFold fallback). Water.ps opens the SSR result from
// ResourceDescriptorHeap[LightCB.ssrResultIdx], and SSRComposite.cs adds it to HDR on water frames.

// Ray-traced reflection: RTReflectionPass's denoised output, SAME frame (that pass runs between
// DecalPass and this one). .rgb = reflected radiance (pre-Fresnel), .a = rtConf. Folded in
// specRadiance space so it shares the Fresnel / envBRDF / AO / NPR path of the probe and sky
// tiers; rtConf == 0 (miss, disabled, black fallback) leaves those tiers alone.
// No register: a bindless index in LightCB (it costs no root-signature DWORD), fetched from
// ResourceDescriptorHeap[rtReflectionIdx] at the use site. t44 stays RESERVED.

// DDGI bindings: multi-volume and all bindless, opened at the use site inside the
// `ddgiEnabled != 0 && ddgiVolumeCount > 0` gate:
//   1. the volume buffer from LightCB.ddgiVolumeBufIdx (wave-uniform: no NonUniformResourceIndex);
//   2. per volume slot `vi`, its irradiance / depth / probe-data resources at
//      vol.irradianceBaseIdx / depthBaseIdx / probeDataBaseIdx + vi. NonUniformResourceIndex IS
//      required there: `vi` is a per-pixel loop variable, and omitting it mis-samples
//      multi-volume scenes.
// A slot is active iff its flags & 1u; inactive slots hold type-correct null descriptors
// (DDGIVolumeManager::WriteSlotNullDescriptors), so a stale slot is inert, not undefined.
// t28..t40 stay RESERVED.
// DDGISampling.hlsli takes every resource as a parameter on purpose: its includers bind the
// per-volume resources differently (this pass opens them from ResourceDescriptorHeap,
// DDGIRayTrace.cs and the reflection shaders bind registers), so each caller opens them and
// passes them in.
#include "DDGISampling.hlsli"

// Aerial Perspective 3D LUT (Hillaire 2020). xy = screen uv, z = quadratic depth slice over
// 0..aerialMaxDistKm. rgb = pre-multiplied inscatter, a = transmittance.
Texture3D<float4> gAerialPerspective : register(t20, space0);

// CSM shadow cascade maps
Texture2DArray<float> gShadowCascades : register(t9, space0);

// The per-object (player) directional shadow map (PerObjectShadowPass), the hair (bangs -> face)
// shadow map and the RT sun-shadow mask (RTShadowPass, full screen, 1 = lit) have no register:
// shadow.hlsli opens each from ResourceDescriptorHeap[] inside its own gate:
//   SamplePerObjectShadow -> perObjShadowIdx   (perObjShadowParams.x != 0)
//   SampleHairShadow      -> hairShadowIdx     (hairShadowParams.x != 0, faces)
//   RT_SHADOW_MASK block  -> rtShadowMaskIdx   (rtShadowEnabled != 0)
// t41..t45 stay RESERVED.

#include "sampler_heap.hlsli"   // samplers from the sampler heap
#include "bindless.hlsli"       // typed bindless handles
#define gSampler SmpFixed(SMP_LINEAR_MIP_POINT_CLAMP)
#define gIBLSampler SmpFixed(SMP_LINEAR_WRAP)
#define gShadowSampler SmpCmp(SMP_CMP_LINEAR_BORDER_WHITE_GE)

// Reflection-probe brightness normalization: needs gIrradiance, gIBLSampler, EvalSH2, LightCB
// and DDGISampling, all declared above.
#include "probe_normalization.hlsli"

// RT4, the GBuffer "extra" slot: per-pixel shading-model data (anisotropy, subsurface, clear
// coat, the face SDF channels and marker, the NPR direct-additive mask). The default GBuffer PS
// writes zero; custom shaders opt in. Emissive is not here: it seeds HdrSceneColor (GBuffer
// target 5), and RT0 carries baseColor only.
Texture2D<float4> gExtraGBuffer : register(t17, space0);

// FoliageTransmission (two-sided foliage) lives in brdf.hlsli beside SubsurfaceTransmission, so
// the forward path shares it.


#include "surface_eval.hlsli"   // SurfaceShading, EvalSurfaceBRDF, SurfaceDiffuseNoL, EvalSurfaceTransmission

#if NPR_PASS
// Bindless texture array (the GBuffer pass's table; LightingPass binds it at root param
// Gfx::BindlessTex). The NPR pass samples each material's toon ramp from it by id
// (gMaterials[matIdx].toonRampTex): this deferred pass has no per-draw textures.
Texture2D g_AllTextures[] : register(t0, space2);

// Per-material toon ramp: a custom texture of the part shader (Face/Hair/Eye/Base/...) - the one
// it names `//@engine toonRamp`, else its t3 - whose id the material packer puts in toonRampTex.
// The ramp ROW layout (TOON_RAMP_V_*) and the per-light toon maths (SigmoidSharp, the half-lambert
// form term, the scene-light coupling) live in toon_eval.hlsli, shared with the forward path.
#include "toon_eval.hlsli"

// ==== The two lighting invariants: read before touching any term below ====
// Checker: tools/check_vis_linear.ps1.
//
// VIS-LINEAR. A PHYSICAL VISIBILITY - any [0,1] scalar from an occlusion query against real
// geometry:
//     ComputeShadowFactor / ComputeShadowFactorExternalOnly (shadow.hlsli)
//     SampleSpotShadow / SamplePointShadow                  (punctual_shadow.hlsli)
//     SampleHairShadow                                      (shadow.hlsli)
//     the RT sun-shadow mask                                (shadow.hlsli)
// may appear ONLY as a multiplicative factor on a direct-light term, or on a radiance that feeds
// one. It may NEVER be an argument to a texture sample, pow, smoothstep, step, SigmoidSharp, or
// any identifier matching *Ramp*.
//
//   The only sanctioned exception: an ENDPOINT-PINNED function - exactly 0 at v == 0 and exactly
//   1 at v == 1 - may transform it, because a fully occluded pixel then still gets exactly zero
//   energy. `ToonVisibility` (lighting_types.hlsli) and `lerp(tint, 1, v)` qualify. `SigmoidSharp`
//   does NOT: SigmoidSharp(0, 0.5, 0.5) == 1.778e-4, not 0.
//   Every such call site must carry `// @vis-linear` plus the endpoint proof.
//
// ARTISTIC / FORM SHADING - FaceSdfTerminator (face_sdf.hlsli), the sharpened half-lambert, the
// hair screen mark (FringeShadowMark.ps.hlsl) - is a DIFFERENT quantity. It may index anything,
// and it may NEVER be the sole gate on energy.
//
// RAD-GATED, the dual (grep the PRESENT token, not the absent one). VIS-LINEAR constrains how a
// visibility identifier is USED, so it is blind to a direct term that carries none. Hence:
//   Every expression that reads a light's radiance (lightColor, toonLightColor, radiance,
//   nprRadiance, coupledMainLight) must terminate in exactly one direct-lighting accumulation,
//   and none of those tokens may be assigned to an indirect bucket or to `s.emissive`.
//   GBuffer-stage clause: asset/Shaders/** and the GBuffer *.ps.hlsl set run BEFORE any shadow
//   exists, so they may not reference lightColor at all - a term written there has its
//   visibility pinned to 1.
// The checker enforces both: a rule kept only in prose does not stop a leak written four lines
// below it.

// SigmoidSharp (toon_eval.hlsli) is DanbaidongRP's toon threshold,
// 1 / (1 + 100000^(-3 * sharp * (x - center))): an e-base slope of ~34.54 * sharp, with fp32
// overflow resolving to exact 0/1 at the extremes. It is NOT endpoint-pinned:
// SigmoidSharp(0, 0.5, sharp) is 1.5e-1 at sharp = 0.1 and 1.8e-4 at sharp = 0.5. Use it on FORM
// terms only. A physical visibility fed through it into a ramp U leaks sun-coloured light into
// fully shadowed pixels - mostly through the ramp's authored non-black floor
// (TOON_RAMP_V_DIRECT_SHADOW), which is O(1).
//
// The NPR model transcribes a live Blender dump of the "Arknights: Endfield_PBRToonBase" node
// group. Where a constant looks arbitrary the node graph holds that number - do NOT clean it up:
//   * base 100000 and factor -3 in SigmoidSharp; not e, not -1.
//   * diffuse normalisation is 1/PI multiplying the RAMP, with NO second NdotL.
// The model runs in this deferred pass rather than in the GBuffer custom PS because three Blender
// operations multiply the ENTIRE composite (specular and ambient included); folded into albedo in
// the custom PS they would reach diffuse only.

// Toon shading context: everything needed to resolve a stylized shadow term for ONE light,
// gathered once per pixel so the clustered loop shapes every point/spot light the way the main
// directional is shaped, instead of adding plain Lambert on top of the ramped sun.
struct NPRToonCtx
{
    int    rampIdx;         // bindless toon ramp, -1 = none (use the flat ramp colors)
    float3 rampColShadow;   // fallback when rampIdx < 0
    float3 rampColLit;

    // ANY face marker (V1 baked or V2 SDF); `faceSDF` is the narrower "this pixel resolves a
    // per-light SDF sweep". `face` gates the DIRECT SPECULAR: a face's terminator is AUTHORED (an
    // SDF lightmap) and its shading normal is smoothed and sphere-blended, so the GGX lobe, which only
    // knows N.L, cannot find the authored boundary; ungated, the sun's highlight sits on the SDF's
    // shadow side. See the spec gates in the toon composite and in AccumClusteredLight.
    bool   face;
    bool   faceSDF;         // V2 face: resolve the SDF per light
    bool   faceAzimuth;     // V2 marker 3.0: atan2 azimuth threshold, not cosine
    float  sdfRight;        // RT4.r: mirror half for dot(L_xz, faceRight_xz) >= 0
    float  sdfLeft;         // RT4.b: mirror half for < 0
    float2 faceFrontXZ;
    float2 faceRightXZ;
    float  faceOffset;
    float  faceSharp;
    float  faceStrength;

    float  ndlSharp;        // non-face / V1 fallback: sharpened half-lambert
    float  ndlThreshold;

    // Ramp has >= 2 rows. A 1-D (Nx1) ramp - what most game rips ship, e.g. every Arknights:
    // Endfield `_RD` - has no spec row, so the TOON_RAMP_V_DIRECT_SPEC lookup would return the
    // DIFFUSE ramp and paint a coloured glint over the whole character. The shadow rows are
    // unaffected: the sampler clamps, so any V on a 1-row texture reads row 0, the single ramp.
    bool   rampHasSpecRow;

    // Per-material energy contract.  The ramp replaces angular response only;
    // physical light chroma/intensity are coupled separately and diffuse keeps
    // its Lambert 1/PI normalization.
    float  directScale;
    float  additionalScale;
    float  lightColorWeight;
    float  lightIntensityWeight;
    float  referenceLightLuminance;
};

// Split a physically-authored light into chroma and scalar energy.  Artists can
// retain the scene hue while partially decoupling a toon character from extreme
// PBR intensities; defaults (both weights = 1) reproduce the source light.
float3 NPRCoupleSceneLight(float3 sceneLight, NPRToonCtx c)
{
    return ToonCoupleSceneLight(sceneLight, c.lightColorWeight, c.lightIntensityWeight,
                                c.referenceLightLuminance);
}

// Stylized shadow term (1 = lit) for a single light.
//   dirTowardLight  unit vector from the shaded point TOWARD the light.
// V2 faces resolve the real SDF sweep for THIS light; everything else (non-face
// NPR parts, and V1 faces whose baked terminator only encodes the sun's yaw) falls
// back to the sharpened half-lambert, which is what DanbaidongRP does for
// additional lights anyway.
float NPRToonShadowTerm(NPRToonCtx c, float3 N, float3 dirTowardLight)
{
    if (c.faceSDF)
        return FaceSdfTerminator(c.sdfRight, c.sdfLeft,
                                 c.faceFrontXZ, c.faceRightXZ, dirTowardLight,
                                 c.faceOffset, c.faceSharp, c.faceStrength,
                                 c.faceAzimuth);

    return ToonHalfLambertTerm(N, dirTowardLight, c.ndlThreshold, c.ndlSharp);
}

// Ramp color for an ADDITIONAL light's shadow term.
float3 NPRToonAddRamp(NPRToonCtx c, float term)
{
    if (c.rampIdx >= 0)
    {
        Texture2D ramp = g_AllTextures[NonUniformResourceIndex(c.rampIdx)];
        return ramp.SampleLevel(gSampler, float2(saturate(term), TOON_RAMP_V_ADD_SHADOW), 0).rgb;
    }
    return lerp(c.rampColShadow, c.rampColLit, saturate(term));
}
#endif

// Screen-space AO (XeGTAO, one-frame latency). 1.0 = fully lit, 0.0 = fully occluded.
Texture2D<float>  gSSAO      : register(t18, space0);

// Per-material data, indexed by the material index packed in normal.a.
StructuredBuffer<MaterialGPUData> gMaterials : register(t12, space0);

// Clustered lighting
#include "cluster_common.hlsli"
#include "light_eval.hlsli"   // the punctual-falloff authority
StructuredBuffer<GPULight>        gLights         : register(t13, space0);
StructuredBuffer<uint>            gLightIndexList : register(t14, space0);
StructuredBuffer<LightGridEntry>  gLightGrid      : register(t15, space0);

// Spot-light shadow atlas (opt-in via LightData::castsShadow): D32_FLOAT slices, reversed-Z
// (closer to the light = larger z).
Texture2DArray<float>       SpotShadowAtlas : register(t21, space0);
// SampleSpotShadow, SamplePointShadow and the spot-shadow views (t22) live in
// punctual_shadow.hlsli, shared with the forward translucent path's clustered loop
// (custom_surface.hlsli).
#include "punctual_shadow.hlsli"

#define PER_OBJECT_SHADOW   // enable SamplePerObjectShadow in shadow.hlsli (bindless perObjShadowIdx)
#define HAIR_SHADOW         // enable SampleHairShadow in shadow.hlsli (bindless hairShadowIdx, NPR faces only)
#define RT_SHADOW_MASK      // enable the RT sun-shadow mask fold in shadow.hlsli (bindless rtShadowMaskIdx)
#include "shadow.hlsli"

// LightingAccum: every accumulator this pass carries. `agg` is the taxonomy
// (lighting_types.hlsli); the npr* fields are the NPR permutation's own buckets. They stay
// separate named fields rather than being reconciled into `agg`: reconciling them changes the
// image.
struct LightingAccum
{
    AggregateLighting agg;
#if NPR_PASS
    float3 nprAmbientColor;   // full-colour ambient (IBL), never ramped
    // Direct specular, split by LIGHT FAMILY. Do not fuse the two: the spec ramp takes its hue and
    // its gate from the sun alone (a punctual light must not supply a hue the sun's geometry switches
    // on), and the sun's half stays UNGATED so each consumer applies the visibility it is entitled to
    // (for the sun, the same possibly-stylized edge as diffuse).
    float3 nprSunSpecUngated; // SUN only. radiance x NdotL, NO visibility.
                              //   The spec-ramp stylizes THIS, and every consumer
                              //   multiplies its own visibility in.
    float3 nprPunctualSpec;   // every clustered point/spot, each already gated by
                              //   its own shadow inside AccumClusteredLight.
    float3 nprIndirSpec;      // indirect half (IBL/probe/traced)
    float3 nprAddColor;       // punctual diffuse, already per-light ramped
    float  nprSunSpecLum;     // visibility-free sun BRDF peak, for the spec-ramp U
#endif
};

LightingAccum InitLightingAccum()
{
    LightingAccum a;
    a.agg = ZeroLighting();
#if NPR_PASS
    a.nprAmbientColor  = float3(0, 0, 0);
    a.nprSunSpecUngated = float3(0, 0, 0);
    a.nprPunctualSpec   = float3(0, 0, 0);
    a.nprIndirSpec     = float3(0, 0, 0);
    a.nprAddColor      = float3(0, 0, 0);
    a.nprSunSpecLum    = 0.0;
#endif
    return a;
}

// One clustered punctual light. The wave-uniform fast path and the divergent fallback both call
// it, so the body exists once. Accumulates into `acc`; the NPR permutation also shapes the light
// through toonCtx.
void AccumClusteredLight(
    GPULight light,
    float3   worldPos,
    float3   worldDx,
    float3   worldDy,
    float3   N,
    float3   V,
    float3   F0,
    float3   albedo,
    float    roughness,
    float    metalness,
    // AO, for the subsurface back-scatter half (UE gates it with AO; the
    // in-scatter half is a see-through term and is not occluded). Ignored by
    // every other model.
    float    ao,
    // False for a material that opted out of receiving shadows: its spot / point shadow lookups are
    // skipped, like the sun's.
    bool     receiveShadow,
    // The pixel's shading model and its per-pixel data (two-sided foliage's tint/wrap, anisotropy's
    // tangent frame, subsurface's tint/opacity/wrap). Passed in every permutation: the NPR one
    // resolves these PBR-path models inert.
    SurfaceShading sm,
    inout LightingAccum acc
#if NPR_PASS
    , NPRToonCtx toonCtx
#endif
)
{
    // Off, out of range, outside its cone, or behind a surface that neither transmits nor wraps:
    // nothing to add (PunctualLightReach in surface_eval.hlsli, shared with the forward loop).
    PunctualReach r;
    if (!PunctualLightReach(light, worldPos, N, sm, r)) return;
    const float3 Ll         = r.L;
    const float  NdL        = r.NdL;
    const float  atten      = r.atten;
    const float  spotFactor = r.spotFactor;

    const float3 sourceLight = light.color * light.intensity;
    float punctualVisibility = 1.0;

    // Opt-in spot shadow: when the light owns a slice in the atlas, modulate
    // radiance by the comparison-sampled visibility term.
    if (receiveShadow && light.type == 2 && light.shadowSliceIdx != 0xFFFFFFFFu)
        punctualVisibility = SampleSpotShadow(worldPos, worldDx, worldDy, N, light.position,
                                              light.spotAngle, light.shadowSliceIdx);
    // Opt-in point shadow: omnidirectional cube sampled by direction.
    else if (receiveShadow && light.type == 1 && light.shadowSliceIdx != 0xFFFFFFFFu)
        punctualVisibility = SamplePointShadow(worldPos, N, light.position, light.radius,
                                             light.shadowSliceIdx);

    // Radiance WITHOUT visibility. The shadow term is handed to AccumulateDirect
    // instead of pre-folded here, so that the one multiply site named by the
    // VIS-LINEAR invariant is also the only place it happens.
    float3 radiance = sourceLight * atten * spotFactor;
    // @vis-linear — punctualVisibility only ever reaches AccumulateDirect (and,
    // below, a plain multiply on the NPR radiance). It never indexes anything.
    LightVisibility vis = MakeVisibility(punctualVisibility, 1.0);

    // The transmitted and the reflected terms (surface_eval.hlsli's
    // EvalPunctualSurface). False when only transmission reaches this pixel - a
    // foliage pixel lit purely from behind - which is still accumulated.
    DirectLighting d;
    float3 specL, kDL;
    // The two AccumulateDirect calls are deliberate: on the transmission-only path the compiler then
    // knows diffuse and specular are zero and folds them; one merged call keeps phi(value, 0) adds
    // alive.
    if (!EvalPunctualSurface(r, radiance, N, V, F0, albedo, roughness, metalness, ao,
                             sm, d, specL, kDL))
    {
        AccumulateDirect(acc.agg, d, vis);
        return;
    }
    AccumulateDirect(acc.agg, d, vis);

    // The wrap exemption ends here. Only a wrapping SUBSURFACE pixel reaches this line with NdL == 0
    // (the reach gate is `NdL > 0 || wrapsOn`), and the NPR tail below must not run for it:
    // NPRToonAddRamp's dark end is an authored NON-BLACK floor accumulated with no N.L factor, so a
    // lamp BEHIND the surface would hand it an O(1) share of its energy - a form term as the sole
    // gate on energy. The PBR half needs no guard: WrapDiffuseNoL is exactly 0 once rawNdL <= -wrap,
    // and specular carries the true saturated NdL. For every other model NdL > 0 here already.
    if (NdL <= 0.0) return;

#if NPR_PASS
    // Scalar light energy (no albedo) for the NPR diffuse rebuild.
    float3 nprRadiance = NPRCoupleSceneLight(sourceLight, toonCtx)
                       * atten * spotFactor * punctualVisibility;
    // Punctual diffuse rides nprAddColor below, ramped per light with its own terminator: it never
    // shares the sun's ramp colour.

    // THIS light's stylized form shadow, hoisted out of the diffuse term because the face specular
    // gate needs it too: this light's own term (its own SDF sweep side), not the sun's.
    float addTerm = NPRToonShadowTerm(toonCtx, N, Ll);

    // This light's shadowed direct specular, rebuilt from the bare BRDF and the COUPLED punctual
    // radiance, so Scene Light Intensity/Color Weight and Additional Light Scale scale diffuse and GGX
    // together. A FACE also gates it by this light's own terminator: the face's form shadow is an
    // authored SDF, decoupled from the smoothed shading normal GGX runs on, so ungated a point light
    // puts a highlight on the face's shadow side. Non-face pixels keep 1.0: their form shadow is a
    // function of N, and the reference (Blender `PBRToonBase`) gates body specular by N.L and the
    // cast shadow only.
    float specForm = toonCtx.face ? addTerm : 1.0;
    acc.nprPunctualSpec += specL * nprRadiance * NdL
                         * toonCtx.additionalScale * specForm;

    // Additional-light diffuse, albedo-free (the composite multiplies albedo). Each light goes through
    // the toon ramp with its own terminator - for a V2 face its own SDF sweep side - so the sum is
    // never ramped as a whole. Without a ramp texture NPRToonAddRamp takes the material's two ramp
    // colours. The ramp replaces NdL but NOT the Lambert 1/PI normalization.
    acc.nprAddColor += kDL * nprRadiance * NPRToonAddRamp(toonCtx, addTerm)
                     * (toonCtx.additionalScale / PI);
#endif
}

// AccumClusteredLight's accumulator arguments, one definition for both call sites.
#if NPR_PASS
    #define CLUSTERED_LIGHT_ACCUM_ARGS acc, toonCtx
#else
    #define CLUSTERED_LIGHT_ACCUM_ARGS acc
#endif

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

struct LightingTargets
{
    float4 reflectionSource : SV_Target0;
    float4 finalColor       : SV_Target1;
};

LightingTargets MakeLightingTargets(float3 reflectionSource, float3 finalColor)
{
    LightingTargets o;
    o.reflectionSource = float4(reflectionSource, 1.0);
    o.finalColor       = float4(finalColor,       1.0);
    return o;
}

#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float3 ReconstructWorldPos(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, invViewProj);
}

LightingTargets main(PSIn i)
{
    float3 albedo    = gAlbedo.Sample(gSampler, i.uv).rgb;
    // Depth is a discrete surface sample, never a filtered blend of surfaces.
    float depth = gDepth.Load(int3(int2(i.pos.xy), 0));
    float3 worldPos = ReconstructWorldPos(i.uv, depth);
    // Capture before material early-outs and light-list divergence.
    float3 worldDx = ddx(worldPos);
    float3 worldDy = ddy(worldPos);

    // ---- Global view-mode override (LightCB.viewMode) ----
    // Both modes bypass the BRDF:
    //   Unlit     -> flat base color; emissive still arrives via the additive blend onto the
    //                GBuffer-seeded HdrSceneColor.
    //   Wireframe -> constant teal. Geometry PSOs rasterize edges only (FILL_MODE_WIREFRAME) and only
    //                edge pixels carry a stencil id, so teal lines land on a black background (the
    //                background-filling passes are hidden in this mode).
    if (viewMode == VIEW_MODE_UNLIT)
        return MakeLightingTargets(albedo, albedo);
    if (viewMode == VIEW_MODE_WIREFRAME)
        return MakeLightingTargets(WIREFRAME_COLOR, WIREFRAME_COLOR);

    // Path-trace path: a GBuffer pixel shows the path tracer's radiance. NPR pixels keep their own
    // model - the ramp, face SDF terminator, scene-light coupling and brightness floor are art
    // direction the PT's GGX cannot reproduce - so the NPR permutation never takes this exit and
    // shades from the raster inputs that stay live on this path (CSM, DDGI, clustered lights), as the
    // ReSTIR paths do (RTGI drops toon receivers, DI folds non-NPR only). Camera-primary PT draws
    // every pixel through the PBR variant.
#if !NPR_PASS
    if (!BindlessNone(gPtResultIdx) && gRenderPathMode == 4u)
    {
        Texture2D<float4> pt = BindlessTex2D<float4>(gPtResultIdx);
        float3 radiance = pt.Load(int3(int2(i.pos.xy), 0)).rgb;
        return MakeLightingTargets(radiance, radiance);
    }
#endif
    float4 normalRaw = gNormal.Sample(gSampler, i.uv);
    float3 N         = normalize(normalRaw.rgb * 2.0 - 1.0);
    // normal.a: the material index and the XeGTAO-exclude bit (core/gbuffer_codec.hlsli).
    // Lighting itself doesn't care about the flag.
    uint matIdx       = GBufferDecodeMatIdx(normalRaw.a);

    // ---- GBuffer channel views (VIEW_MODE_BASECOLOR..DEPTH) ----
    // Raw GBuffer contents, no BRDF. Placed BEFORE the shading-model routing so unlit materials show
    // their channels too. Sky pixels never reach this stencil-gated pass, so the background stays
    // black. The output still goes through the tonemapper (monotonic, so the ramps stay readable).
    if (VIEW_MODE_IS_GBUFFER_DEBUG(viewMode))
    {
        float3 dbg = float3(0.0, 0.0, 0.0);
        if      (viewMode == VIEW_MODE_BASECOLOR)   dbg = albedo;
        else if (viewMode == VIEW_MODE_WORLDNORMAL) dbg = N * 0.5 + 0.5;
        else if (viewMode == VIEW_MODE_DEPTH)
        {
            // Reversed-Z raw depth: 1 = near plane, EXACTLY 0 = sky. A 4th
            // root spreads the far field, which raw reversed-Z crushes into
            // the last few float steps, into a readable white-near ramp.
            const float raw = gDepth.Sample(gSampler, i.uv);
            dbg = pow(saturate(raw), 0.25).xxx;
        }
        else
        {
            const float4 surfaceDbg = gSurface.Sample(gSampler, i.uv);
            if      (viewMode == VIEW_MODE_ROUGHNESS) dbg = surfaceDbg.rrr;
            else if (viewMode == VIEW_MODE_METALLIC)  dbg = surfaceDbg.ggg;
            else /* VIEW_MODE_AO */
                dbg = (surfaceDbg.b * gSSAO.Sample(gSampler, i.uv).r).xxx;
        }
        return MakeLightingTargets(dbg, dbg);
    }

    // Shading-model routing for custom materials: Unlit exits with baseColor only (its emissive
    // arrives via the additive blend onto the GBuffer-seeded HdrSceneColor). Every other model shades
    // below; Anisotropic, Subsurface, ClearCoat and two-sided foliage are runtime branches of the PBR
    // variant.
    const uint runtimeShadingModel = gMaterials[matIdx].customShadingModel;

    if (runtimeShadingModel == SHADING_MODEL_UNLIT)
    {
        const float3 c = albedo;
        return MakeLightingTargets(c, c);
    }

#if UNLIT
    // Unlit variant: baseColor only; emissive arrives via the additive blend.
    {
        const float3 c = albedo;
        return MakeLightingTargets(c, c);
    }
#else
    float4 surface   = gSurface.Sample(gSampler, i.uv);


    float roughness   = max(surface.r, 0.045);
    float metalness   = surface.g;
    float bakedAO     = surface.b;
    float ssao        = gSSAO.Sample(gSampler, i.uv).r;
    float ao          = bakedAO * ssao;    // combine baked + screen-space AO
    float reflectance = surface.a;

    float3 V     = normalize(cameraPos - worldPos);
    float3 L     = normalize(-lightDir);
    float  NdotL = saturate(dot(N, L));
    float  NdotV = saturate(dot(N, V));

    float  f0Scalar = 0.16 * reflectance * reflectance;
    float3 F0 = lerp(float3(f0Scalar, f0Scalar, f0Scalar), albedo, metalness);

    // RT4, read ONCE for every permutation, ahead of the BRDF: the shading-model resolve below
    // decodes it and the sun's BRDF depends on the result.
    const float4 extraGB = gExtraGBuffer.Sample(gSampler, i.uv);

    // ---- Shading model, resolved once for the sun AND every punctual light ----
    // Anisotropic, Subsurface and ClearCoat are ENGINE shading models: their knobs are named
    // MaterialGPUData fields (Inspector-editable on a plain material), and their per-pixel half is
    // decoded from RT4 with the GBuffer pass's codec. Every branch leaves the struct INERT for a
    // Standard pixel (isotropic BRDF, no wrap, no transmission), so the common path costs one
    // uniform compare.
    SurfaceShading sm = InitSurfaceShading();
    sm.model = runtimeShadingModel;
    {
        MaterialGPUData smMat = gMaterials[matIdx];
        // FACE pixels carry no model data in RT4: MS_EncodeGBuffer does not stamp .r/.g when the marker is
        // set, because a V2 face needs all four channels (the two SDF halves, the hair screen mark, the
        // marker). Decoding them would read an SDF half as an anisotropy azimuth or a subsurface opacity.
        // This is the decoder half of that contract.
        const bool smFacePixel = (extraGB.a > 0.5);
        if (runtimeShadingModel == SHADING_MODEL_ANISOTROPIC)
        {
            // RT4.r = the direction as an angle in N's tangent plane, RT4.g = the signed anisotropy; the
            // decoded frame is exactly orthonormal (no Gram-Schmidt). A face falls back to ISOTROPIC: a
            // deferred pass has no tangent frame to recover, and an arbitrary azimuth would give the face a
            // highlight that follows nothing on the mesh.
            sm.anisotropy = smFacePixel ? 0.0 : clamp(extraGB.g, -1.0, 1.0);
            SM_AnisoFrame(N, smFacePixel ? 0.0 : extraGB.r,
                          sm.tangentWS, sm.bitangentWS);
        }
        else if (runtimeShadingModel == SHADING_MODEL_SUBSURFACE)
        {
            // Tint = material colour x material strength x the per-pixel mask in RT4.g, so `any(sssTint > 0)`
            // downstream is the single "is this model live" test - a material that never authored a
            // subsurface colour is bit-identical to Standard. On a face the per-pixel half degrades to the
            // MATERIAL values (mask 1, the authored opacity): unlike a tangent direction, an SSS opacity has a
            // meaningful material-level default.
            const float sssMask = smFacePixel ? 1.0 : saturate(extraGB.g);
            sm.sssTint    = max(smMat.subsurfaceColor, 0.0)
                          * max(smMat.subsurfaceStrength, 0.0)
                          * sssMask;
            sm.sssOpacity = smFacePixel ? saturate(smMat.subsurfaceOpacity)
                                        : saturate(extraGB.r);
            sm.sssWrap    = saturate(smMat.subsurfaceWrap);
        }
        else if (runtimeShadingModel == SHADING_MODEL_CLEARCOAT)
        {
            // RT4.r = coat strength, RT4.g = the COAT's roughness. A face keeps its own channels, so it
            // degrades to the material-level values (both have a meaningful material default).
            sm.clearCoat          = smFacePixel ? saturate(smMat.clearCoat)
                                                : saturate(extraGB.r);
            sm.clearCoatRoughness = smFacePixel ? saturate(smMat.clearCoatRoughness)
                                                : saturate(extraGB.g);
        }
        else if (runtimeShadingModel == SHADING_MODEL_TWOSIDED_FOLIAGE)
        {
            // The named subsurface fields. The material packer (WriteMaterialSlot) copies a custom shader's
            // foliage parameters into them with their unset defaults applied (two shipping Tokyo materials
            // author their tint as a reflected b8 parameter), so plain and custom materials read the same
            // fields here.
            sm.foliageWrap = saturate(smMat.subsurfaceWrap);
            sm.foliageTint = max(smMat.subsurfaceColor, 0.0)
                           * max(smMat.subsurfaceStrength, 0.0);
        }
    }

    // Cook-Torrance BRDF for the sun: anisotropic GGX when the shading model asks for it, else the
    // isotropic lobe.
    float3 specular, kD;
    EvalSurfaceBRDF(sm, N, V, L, F0, roughness, metalness, albedo, specular, kD);

    float  sunVisibility = ComputeShadowFactor(worldPos, N, i.pos.xy);

    // FACE pixels (RT4.a marker) must not receive the character's own projected shadows. The
    // per-object player map carries exactly those casters (bangs -> face, face -> face, body -> face)
    // and the RT sun mask traces the character too, so a face takes the cascades-only factor: the
    // external world's shadow. The bangs' shadow arrives only through the artistic hair term (screen
    // mark / fitted hair map) and the face's self-shadow is the SDF terminator. Overridden here, where
    // sunVisibility is born, so every consumer sees the same value.
    // Outside `#if NPR_PASS` on purpose: "is this a face" is a GBuffer property (RT4.a, written by
    // the face surface shader), not the material's shaderType - face parts authored as PBR (e.g.
    // Mouth, EmotionsA/B) need it too. Unlit materials exited far above, so this costs the PBR
    // permutation one branch on an already-sampled value.
    if (extraGB.a > 0.5)
        sunVisibility = ComputeShadowFactorExternalOnly(worldPos, N, i.pos.xy);

#if NPR_PASS
    const float4 nprExtraEarly = extraGB;
    // PT-lit toon receivers: on the path-trace path the two inputs the toon model takes from the
    // raster tiers come from the path tracer instead - PathTrace.cs.hlsl's NprAmbient, denoised by
    // PathTraceNprDenoise.cs.hlsl: .a the traced sun visibility, .rgb the traced ambient (folded in
    // with the isotropic taps below). The model itself is unchanged. Faces keep the external-only
    // cascades above: their self-shadow is the SDF terminator.
    float4 nprPt = float4(0, 0, 0, -1);
    if (!BindlessNone(gNprPtAmbientIdx) && gRenderPathMode == 4u)
    {
        Texture2D<float4> nprPtTex = BindlessTex2D<float4>(gNprPtAmbientIdx);
        nprPt = nprPtTex.Load(int3(int2(i.pos.xy), 0));
    }
    const bool nprPtLive = nprPt.a >= 0.0;
    if (nprPtLive && extraGB.a <= 0.5)
        sunVisibility = nprPt.a;
#endif
    // Per-material receive-shadow opt-out (MAT_FLAG_DISABLE_RECEIVE_SHADOW). It covers every light:
    // AccumClusteredLight skips its spot / point shadows too, as the forward path does.
    const bool receiveShadow =
        (gMaterials[matIdx].materialFlags & MAT_FLAG_DISABLE_RECEIVE_SHADOW) == 0u;
    if (!receiveShadow)
        sunVisibility = 1.0;

    // The colour Transmissive glass leaves on the sun (RTShadowTint), where the RT mask is part of
    // sunVisibility: not on a face (cascades only), a PT-lit toon receiver or a surface that
    // receives no shadow.
    float3 sunTint = 1.0;
    if (receiveShadow && extraGB.a <= 0.5)
        sunTint = RTShadowTint(i.pos.xy);
#if NPR_PASS
    if (nprPtLive && extraGB.a <= 0.5)
        sunTint = 1.0;
#endif

    // Volumetric cloud shadow, applied AFTER the two overrides above on purpose: they decide which
    // CASTERS a surface accepts, while a cloud in front of the sun changes how much sun there is (a
    // face or a no-receive-shadow material staying lit while the ground darkens reads as glowing).
    // Every sun consumer - the direct lobe, transmission, the NPR ramp - takes sunVisibility from
    // here. Returns 1.0 when no map is bound (LightCB cloudShadowIdx == 0).
    sunVisibility *= SampleCloudShadow(worldPos, gSampler);

    // ---- Directional light (PBR for both paths) ----
    // @vis-linear — sunVisibility reaches AccumulateDirect and nothing else on
    // this path. The toon block reads the same value (as directVisibility) and the debug views
    // read `sunVis`, so the sun's shadow has exactly ONE representation in this shader.
    LightingAccum   acc    = InitLightingAccum();
    LightVisibility sunVis = MakeVisibility(sunVisibility, 1.0);

    float3 diffuse = kD * albedo / PI;

    DirectLighting sun = ZeroDirect();
    // Diffuse takes the shading model's N.L (wrapped past the terminator for Subsurface, plain
    // saturate otherwise). Specular keeps the true N.L in every model: a wrapped highlight would
    // survive onto the shadow side, the artifact wrap exists to avoid on diffuse.
    sun.diffuse  = diffuse  * lightColor * SurfaceDiffuseNoL(sm, dot(N, L));
    sun.specular = specular * lightColor * NdotL;
    // Sun transmitted THROUGH the surface (a leaf's far side, an ear held up to the light). It shares
    // sunVis with the reflected term: a leaf in the canopy's shade must not glow, nor a backlit ear
    // indoors.
    if (SurfaceHasTransmission(sm))
        sun.transmission = EvalSurfaceTransmission(sm, N, V, L, ao) * lightColor;
    sun.diffuse      *= sunTint;
    sun.specular     *= sunTint;
    sun.transmission *= sunTint;

    AccumulateDirect(acc.agg, sun, sunVis);

#if NPR_PASS
    // NPR accumulators on `acc`, directional vs. ambient so IBL keeps its colour:
    //   nprAmbientColor  : full-colour ambient diffuse (IBL irradiance, no ramp), so cool IBL still
    //                      cools skin exactly as on PBR.
    //   nprSunSpecUngated: the SUN's direct specular, radiance x NdotL, NO visibility: the toon block
    //                      applies its own (possibly stylized) edge. Written below, once the toon
    //                      context's coupling exists.
    //   nprPunctualSpec  : clustered point/spot specular, each gated per light.
    //   nprIndirSpec     : indirect specular, so the toon block ramp-boosts ONLY the direct
    //                      highlight and passes the environment reflection through (DanbaidongRP's
    //                      PostEvaluate keeps indirect specular out of the spec ramp).
    //   nprSunSpecLum    : sun-BRDF luminance (F * D * V only: NO lightColor, NdotL or shadow) for the
    //                      spec-ramp U. Matches DanbaidongRP's `specRange = saturate(DV)`: the column
    //                      is picked by proximity to the highlight PEAK, independent of light
    //                      intensity and visibility.
    //   nprAddColor      : punctual diffuse, apart from the sun so it gets its own ramp row.
    acc.nprSunSpecLum     = Luminance(specular);

    // ---- Toon context ----
    // Built before the clustered loop, which needs it: every point/spot light resolves its own
    // stylized shadow term (and, for a V2 face, its own SDF sweep side). nprMat / nprExtra are read
    // once and reused by the toon composite.
    MaterialGPUData nprMat  = gMaterials[matIdx];
    float4 nprExtra         = nprExtraEarly;   // sampled once, next to sunVisibility

    NPRToonCtx toonCtx;
    // RT4.a carries the face marker AND its version (face_sdf.hlsli). The tests are thresholds, not
    // ==, because RT4 is FP16 and linearly sampled: a pixel on a face edge can land between marker
    // values.
    bool toonIsFace   = (nprExtra.a > 0.5);
    // Stylized DIRECT-additive mask, on RT4.a of NON-FACE pixels only: face markers are >= 1.0, this
    // encoding is < 0.5 and every face test is `> 0.5`, so the two cannot collide. The toon composite
    // gates it by directVisibility. Do not write it into s.emissive pre-multiplied by lightColor:
    // that reaches HdrSceneColor with no visibility (full sun inside a cast shadow).
    float toonDirectAdd = toonIsFace
                        ? 0.0
                        : saturate(nprExtra.a / DIRECT_ADDITIVE_ENCODE_SCALE);
    bool toonFaceV2   = (nprExtra.a >= 1.5);
    bool toonFaceAzim = (nprExtra.a >= 2.5);   // marker 3.0: atan2 azimuth sweep
    int  toonRampIdx  = nprMat.toonRampTex;
    toonCtx.face          = toonIsFace;
    toonCtx.rampIdx       = toonRampIdx;
    toonCtx.rampColShadow = nprMat.nprShadowRampColor;
    toonCtx.rampColLit    = nprMat.nprDiffuseRampColor;

    // 1-D vs banded ramp. Queried once per pixel; the result is wave-uniform in
    // practice (one material per pixel quad) so the cost is a scalar load.
    toonCtx.rampHasSpecRow = false;
    if (toonRampIdx >= 0)
    {
        uint rw, rh;
        g_AllTextures[NonUniformResourceIndex(toonRampIdx)].GetDimensions(rw, rh);
        toonCtx.rampHasSpecRow = (rh >= 2u);
    }

    // V2 face params live in the material slot, which the deferred pass CAN read - that is what makes
    // a per-light SDF possible. The head frame (HeadForward.xz, HeadRight.xz) of a face that
    // FaceShadingSystem drives comes from the slot's FRAME RECORD (it changes every frame, the
    // material record does not); anything else - an unwired face, a preview - keeps the V2 contract's
    // customParams[0..1] (face_sdf.hlsli). The terminator parameters are the named faceShadow the
    // material packer resolves.
    float4 toonHeadFrame = float4(nprMat.customParams[0].xz, nprMat.customParams[1].xz);
    if ((nprMat.materialFlags & MAT_FLAG_FRAME_RECORD) != 0u && !BindlessNone(gFaceFrameIdx))
    {
        StructuredBuffer<float4> faceFrames = BindlessSBuf<float4>(gFaceFrameIdx);
        toonHeadFrame = faceFrames[matIdx];
    }
    const float2 toonHeadFwdXZ = toonHeadFrame.xy;
    const float2 toonHeadRgtXZ = toonHeadFrame.zw;
    float4 toonFaceSh  = nprMat.faceShadow;
    toonCtx.faceSDF      = toonFaceV2
                        && dot(toonHeadFwdXZ, toonHeadFwdXZ) > 1e-8
                        && dot(toonHeadRgtXZ, toonHeadRgtXZ) > 1e-8;
    toonCtx.faceAzimuth  = toonFaceAzim;
    toonCtx.sdfRight     = nprExtra.r;
    toonCtx.sdfLeft      = nprExtra.b;
    toonCtx.faceFrontXZ  = FaceSdfNormalize2(toonHeadFwdXZ);
    toonCtx.faceRightXZ  = FaceSdfNormalize2(toonHeadRgtXZ);
    toonCtx.faceOffset   = toonFaceSh.x;
    // An unset (0) sharpness falls back to 2.0.
    toonCtx.faceSharp    = (toonFaceSh.y < 1e-4) ? 2.0 : toonFaceSh.y;
    toonCtx.faceStrength = saturate(toonFaceSh.z);

    toonCtx.ndlThreshold = nprMat.nprShadowThreshold;
    toonCtx.ndlSharp     = ToonNdlSharp(nprMat.nprShadowSmooth);
    toonCtx.directScale          = max(nprMat.nprDirectLightScale, 0.0);
    toonCtx.additionalScale      = max(nprMat.nprAdditionalLightScale, 0.0);
    toonCtx.lightColorWeight     = saturate(nprMat.nprSceneLightColorWeight);
    toonCtx.lightIntensityWeight = saturate(nprMat.nprSceneLightIntensityWeight);
    toonCtx.referenceLightLuminance = max(nprMat.nprReferenceLightLuminance, 1e-4);

    // The sun's physical GGX obeys the same authored light-energy controls as the ramp diffuse
    // (NPRCoupleSceneLight, directScale); otherwise lowering either control exposes an unchanged
    // N-dependent GGX lobe on body/face. nprSunSpecLum above stays the bare BRDF peak the spec-ramp
    // lookup uses.
    acc.nprSunSpecUngated = specular
                           * NPRCoupleSceneLight(lightColor * sunTint, toonCtx)
                           * NdotL * toonCtx.directScale;
#endif

    // ---- Clustered point/spot lights ----
#if !NPR_PASS
    // The DI producer already applied its single visibility query linearly.
    // Which models it lights is the shading-model table's `di` column (the
    // same rule RTDI.cs's receiver uses); whether this pixel is isotropic is
    // ours: the decoded anisotropy, 0 on a face pixel.
    const bool diEligible = ShadingModelDiReceives(sm.model, sm.anisotropy == 0.0);
    // Slice-0 alpha 0 means the producer has no estimate for this pixel (no
    // supporting half-res sample, e.g. a 1-px wire over the sky): fall back to
    // the clustered loop below rather than dropping every local light.
    bool diFolded = false;
    if (!BindlessNone(gRestirDiIdx) && diEligible)
    {
        Texture2DArray<float4> di = BindlessTex2DArray<float4>(gRestirDiIdx);
        float4 diDiffuse = di.Load(int4(int2(i.pos.xy), 0, 0));
        if (diDiffuse.a != 0.0)
        {
            DirectLighting direct = ZeroDirect();
            direct.diffuse = diDiffuse.rgb;
            direct.specular = di.Load(int4(int2(i.pos.xy), 1, 0)).rgb;
            if(diDiffuse.a==2 || diDiffuse.a==3)
                return MakeLightingTargets(direct.diffuse+direct.specular,direct.diffuse);
            AccumulateDirect(acc.agg, direct, FullyVisible());
            diFolded = true;
        }
    }
#else
    const bool diFolded = false;
#endif
    if (!diFolded && clusterLightCount > 0)
    {
        float  viewZ  = abs(mul(float4(worldPos, 1.0), viewMatrix).z);
        uint3  coord  = GetClusterCoord(i.uv, viewZ, clusterNearZ, clusterFarZ);
        uint   key    = ClusterIndex(coord);

        // The cluster XY grid is 48x27 (cluster_common.hlsli): 40x40-pixel tiles at 1080p, while a
        // 32- or 64-lane wave covers at most an 8x8 footprint. 40 is a multiple of 8, so at 1080p/4K a
        // wave never straddles an XY tile edge and the fast path hits ~100%; at other resolutions a
        // minority of waves straddle, and Z-slice divergence at depth discontinuities is the only other
        // miss. The fast path puts the grid entry, the light index and the whole 64-byte GPULight load in
        // SCALAR registers. WaveReadLaneFirst of a wave-uniform value is a no-op, so the fast path is
        // bit-identical to the fallback.
        [branch] if (WaveActiveAllEqual(key))
        {
            const uint sKey    = WaveReadLaneFirst(key);
            const uint sOffset = gLightGrid[sKey].offset;
            const uint sCount  = gLightGrid[sKey].count;

            for (uint li = 0; li < sCount; ++li)
            {
                uint sIdx = li;
                if (sOffset != 0xFFFFFFFFu)
                    sIdx = WaveReadLaneFirst(gLightIndexList[sOffset + li]);
                GPULight light  = gLights[sIdx];
                AccumClusteredLight(light, worldPos, worldDx, worldDy, N, V, F0, albedo,
                                    roughness, metalness, ao, receiveShadow, sm,
                                    CLUSTERED_LIGHT_ACCUM_ARGS);
            }
        }
        else
        {
            uint offset = gLightGrid[key].offset;
            uint count  = gLightGrid[key].count;

            for (uint li = 0; li < count; li++)
            {
                uint idx = li;
                if (offset != 0xFFFFFFFFu) idx = gLightIndexList[offset + li];
                GPULight light = gLights[idx];
                AccumClusteredLight(light, worldPos, worldDx, worldDy, N, V, F0, albedo,
                                    roughness, metalness, ao, receiveShadow, sm,
                                    CLUSTERED_LIGHT_ACCUM_ARGS);
            }
        }
    }

    // ---- IBL (Image-Based Lighting) ----
    // Indirect diffuse: DDGI where a volume covers, sky IBL for the rest -
    // (DDGI * coverage) + (sky * (1 - coverage)).
    // Knobs:
    //   iblStrength      - the SKY LIGHT's master scale: sky IBL diffuse and specular here, and the
    //                      sky every traced consumer sees on a miss (DDGI probe rays via
    //                      g_Vol.skyScale, so DDGI's bounce carries it too; RT reflections; the path
    //                      tracer). 0 removes the sky light everywhere; DDGI keeps the sun's and the
    //                      local lights' bounce. NOT reflection probes: IndirectSpecular_AddProbe /
    //                      _ResolveProbeFallback never read it - probes are gated at BAKE time
    //                      (ReflectionProbeComponent.h); a runtime gate would attenuate the sun bounce
    //                      baked into the cubemap and apply iblStrength twice to the bake's sky half.
    //   ddgiDiffuseScale - DDGI's output scale, shared by SSGI and ReSTIR GI. Independent of
    //                      iblStrength; 1 is physical (the probes store E/pi).
    //   ambientColor x ambientIntensity - WorldEnvironment None's fill only (0 in the sky modes and
    //                      in FlatColor, whose ambient is a uniform sky cube), added after the split.
    // Indirect light goes into acc.agg.indirect through AccumulateIndirect, which takes no
    // LightVisibility: a light's shadow cannot darken the sky, by type.
    //
    // tracedSpecularDelta: the final traced fold minus the fallback-only specular. Subtracting it from
    // Target0 removes the traced tier and keeps the sky/probe fallback, direct light, DDGI, AO and
    // emissive.
    float3 tracedSpecularDelta = float3(0, 0, 0);
    // tracedDiffuseDelta: the same for the SSGI near field. Subtracted from Target0, it keeps NEXT
    // frame's scene-colour pyramid free of this frame's SSGI, so the screen march only sees direct
    // light + the DDGI far field. SSGI samples the previous frame's lit colour; left in, its feedback
    // is a geometric series nested inside DDGI's own multi-bounce loop (DDGIRayTrace feeds the
    // previous atlas back at gain 1, prevSafe * PI) - two feedback loops with different time
    // constants, the shape that makes a whole scene "breathe". With the subtraction, SSGI is exactly
    // one bounce on a converged DDGI cache.
    float3 tracedDiffuseDelta = float3(0, 0, 0);
    // tracedDeltaScale: how much `color` has scaled the deltas by the time they are subtracted. The
    // subtraction needs every step from here to the composite to be AFFINE in the traced terms. PBR is;
    // NPR is not. Its uniform scalings are tracked exactly: specIBL re-added at nprSpecularLightScale
    // (default 0.75, Components.h) and the relative brightness ceiling. Its non-affine steps (the spec
    // ramp, the brightness floor) set the scale to 0 - "traced tier kept in Target0" - instead of
    // subtracting a wrong amount: an error here becomes a reflection that reflects a wrong version of
    // itself.
    float tracedDeltaScale = 1.0;
    // The traced reflection. rtReflectionIdx comes from LightCB, so it is wave-uniform: no
    // NonUniformResourceIndex (it would only add a scalarization loop). The renderer writes the index of
    // RTReflectionPass's denoised output (the standalone SSR chain's upsample output in the FullFold
    // fallback) or of LightingPass's 1x1 BLACK fallback, so rtConf (.a) is 0 whenever reflections are off.
    Texture2D<float4> rtReflectionTex = BindlessTex2D<float4>(rtReflectionIdx);
    const float4 tracedReflection = rtReflectionTex.Load(int3(int2(i.pos.xy), 0));
    // The sky views (VIEW_MODE_SKY_VISIBILITY / VIEW_MODE_IBL_ONLY): what the sky-IBL fallback is
    // multiplied by and what it delivers, filled at the end of the block below. Zero when the block
    // does not run (no indirect source, so no sky).
    float3 dbgSkyVisibility = float3(0, 0, 0);
    float3 dbgIblOnly       = float3(0, 0, 0);
    // The flat environment ambient (WorldEnvironment None's fill; 0 in the sky
    // modes and FlatColor): the radiance of a uniform environment. It joins the
    // gate because its BRDF weights (kDibl, the split-sum) are computed in the block.
    const float3 flatAmbientL = ambientColor * ambientIntensity;
    // The indirect block runs when ANY indirect source is live, not only a sky: DDGI, probes, traced
    // reflections, SSGI / ReSTIR GI and the flat ambient are transport paths of their own (with DDGI off
    // and no sky or probes, SSGI must still run). Sky terms degrade to zero by themselves in non-sky
    // modes: gIrradiance/gRadiance are the shared 1x1 black cube and iblUseSH = 0.
    if (iblRadianceMips > 0
        || (ddgiEnabled != 0 && ddgiVolumeCount > 0)
        || reflectionProbeCount > 0
        || tracedReflection.a > 0.0
        || !BindlessNone(ssgiResultIdx) || !BindlessNone(gRestirGiIdx)
        || any(flatAmbientL > 0.0))
    {
        // ---- Indirect diffuse: DDGI -> sky IBL ----
        // The covering DDGI volumes are arbitrated (DDGI_PickVolumes below); their coverage leaves the
        // (1 - coverage) share to the sky-IBL fallback.
        // Toon receivers get an ISOTROPIC ambient - the spherical AVERAGE of the environment, not a
        // directional tap - because the ramp already owns the character's form: interpolated vertex
        // normals would add a second, polygonal gradient (their C1 creases at triangle edges become
        // visible steps once Direct Light Scale drops and ambient dominates). Not a fixed direction
        // either: a (0,1,0) tap returns the field's zenith, under a Rayleigh sky the blue dome peak. The
        // geometry-side DDGI terms (bias offset, backface weight, Chebyshev) stay on the real N: leak
        // prevention is not a shading choice.
        float3 ddgiIrradiance = float3(0, 0, 0);
        float  ddgiCoverage   = 0.0;
        // DDGI's directional sky visibility (DDGI_SampleVolumeSkyMoments), gathered from the same probes
        // as the irradiance (the same two winners, the same per-probe weights) and read twice: about N for
        // the sky's diffuse fallback, along R for its reflection. skyCov 0 = no volume: the sky stays
        // unoccluded.
        float4 skyMoments = float4(1.0, 0.0, 0.0, 0.0);
        float  skyCov     = 0.0;
        if (ddgiEnabled != 0 && ddgiVolumeCount > 0)
        {
            // ddgiVolumeBufIdx comes from LightCB, so it is wave-uniform: no NonUniformResourceIndex.
            StructuredBuffer<DDGIVolumeGPU> ddgiVolumes =
                BindlessSBuf<DDGIVolumeGPU>(ddgiVolumeBufIdx);

            // Arbitration, not averaging: a fade-weighted mean over every covering volume lets a dense
            // interior volume reach at best 50/50 with the coarse one it overrides, and four volumes at the
            // edges of their fades sum to full coverage and kill the sky fallback where all four are worst.
            // DDGI_PickVolumes ranks by authored priority, then probe density, and returns the top two; the
            // winner owns its interior and hands over only across its own boundary fade. The reflection paths
            // share it so they cannot drift apart.
            DDGIVolumePick pick = DDGI_PickVolumes(worldPos, ddgiVolumes);

            float4 bestSample   = float4(0, 0, 0, 0);
            float4 secondSample = float4(0, 0, 0, 0);
            float4 bestSkyM   = float4(1, 0, 0, 0), secondSkyM   = float4(1, 0, 0, 0);
            float  bestSkyCov = 0.0,                secondSkyCov = 0.0;
            // [unroll] over all slots so `vi` stays a compile-time constant for the descriptor opens below;
            // at most two iterations do any work.
            [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
            {
                if (!DDGI_IsPicked(pick, vi)) continue;
                DDGIVolumeGPU vol = ddgiVolumes[vi];
                // `vi` is a per-pixel loop variable, so NonUniformResourceIndex is REQUIRED (unlike the
                // LightCB-carried indices). The types must match the SRVs DDGIVolumeManager creates exactly:
                // Tier 3 reports nothing on a mismatch.
                Texture2D<float4> volIrradiance =
                    ResourceDescriptorHeap[NonUniformResourceIndex(vol.irradianceBaseIdx + vi)];
                Texture2D<float2> volDepth =
                    ResourceDescriptorHeap[NonUniformResourceIndex(vol.depthBaseIdx + vi)];
                StructuredBuffer<DDGIProbeData> volProbeData =
                    ResourceDescriptorHeap[NonUniformResourceIndex(vol.probeDataBaseIdx + vi)];
                // The irradiance AND the probes' sky-visibility moments, with the same per-probe visibility
                // weights.
                float4 s;
                float4 skyM;
                // A toon receiver (every NPR_PASS pixel) samples isotropically.
#if NPR_PASS
                const bool ddgiIsotropic = true;
#else
                const bool ddgiIsotropic = false;
#endif
                s = DDGI_SampleVolumeSkyMoments(worldPos, N, V, vol,
                                                volIrradiance, volDepth,
                                                volProbeData, gIBLSampler, ddgiIsotropic, skyM);
                if (vi == (uint)pick.bestSlot) { bestSample   = s; bestSkyM   = skyM; bestSkyCov   = s.a; }
                else                           { secondSample = s; secondSkyM = skyM; secondSkyCov = s.a; }
            }

            const float4 ddgiResolved =
                DDGI_CombineWinners(bestSample, secondSample, pick);
            ddgiIrradiance = ddgiResolved.rgb;
            ddgiCoverage   = ddgiResolved.a;
            skyMoments = DDGI_CombineSkyMoments(bestSkyM, bestSkyCov, secondSkyM, secondSkyCov, pick, skyCov);
        }

        float3 skyDiff = (iblUseSH != 0) ? EvalSH2(N)
                                         : gIrradiance.Sample(gIBLSampler, N).rgb;
#if NPR_PASS
        {
            // Isotropic sky ambient for toon receivers (every NPR_PASS pixel; see the DDGI sample above).
            // SH path: the exact band-0 average. Cube path: the 6-axis-tap average of the irradiance cube.
            if (iblUseSH != 0)
            {
                skyDiff = EvalSH2Isotropic();
            }
            else
            {
                skyDiff = (gIrradiance.Sample(gIBLSampler, float3( 1, 0, 0)).rgb
                         + gIrradiance.Sample(gIBLSampler, float3(-1, 0, 0)).rgb
                         + gIrradiance.Sample(gIBLSampler, float3( 0, 1, 0)).rgb
                         + gIrradiance.Sample(gIBLSampler, float3( 0,-1, 0)).rgb
                         + gIrradiance.Sample(gIBLSampler, float3( 0, 0, 1)).rgb
                         + gIrradiance.Sample(gIBLSampler, float3( 0, 0,-1)).rgb)
                        * (1.0 / 6.0);
            }
        }
#endif

        // ---- AO split: no double occlusion ----
        // DDGI already holds mid-scale (probe-spacing) visibility through the probe-distance test, so
        // full screen-space AO on the DDGI part stacks the same occlusion twice and crushes interiors.
        // ddgiAONearFieldStrength lerps AO toward 1.0 for the DDGI part only; the sky fallback keeps full
        // AO except where DDGI's sky visibility covers it (below).
        float ddgiAO = lerp(1.0, ao, saturate(ddgiAONearFieldStrength));
        // The sky's diffuse fallback - the hemisphere share DDGI does not own (its fade band, or all of
        // it with no volume) - takes DDGI's sky visibility where DDGI has it: cosine-weighted about N (the
        // sphere average for a toon receiver, whose sky tap is isotropic too). There it also takes the
        // DDGI part's AO, for the reason above. Uncovered (skyCov 0), both reduce to the unoccluded sky
        // with full AO.
        float skyVisDiff = DDGI_SkyVisibility(skyMoments, N, 2.0 / 3.0);
#if NPR_PASS
        skyVisDiff = saturate(skyMoments.x);   // the toon receiver's sphere average
#endif
        skyVisDiff = lerp(1.0, skyVisDiff, saturate(skyCov));
        const float skyAO = lerp(ao, ddgiAO, saturate(skyCov));
        // Here iblStrength scales the sky part only; DDGI's output scale is ddgiDiffuseScale.
        float3 ddgiDiffPart = ddgiIrradiance * ddgiDiffuseScale * ddgiCoverage * ddgiAO;
        float3 skyDiffPart  = skyDiff * saturate(1.0 - ddgiCoverage)
                            * skyAO * skyVisDiff * iblStrength;
        // The far field, DDGI + sky: indirect diffuse as it is without SSGI. SSGI / ReSTIR GI partition
        // the hemisphere against it below.
        float3 farDiffuse   = ddgiDiffPart + skyDiffPart;
        // The same far field without AO: the reflection-probe normalization's
        // "how bright is the indirect light HERE" (probe_normalization.hlsli).
        const float3 localFarIrr = ddgiIrradiance * ddgiDiffuseScale * ddgiCoverage
                                 + skyDiff * saturate(1.0 - ddgiCoverage)
                                 * skyVisDiff * iblStrength;

        // ---- SSGI: hemisphere PARTITION, not a confidence blend ----
        // SSGIPass resolves part of the hemisphere by marching the screen and reports
        //   .rgb = irradiance integrated over the solid angle it RESOLVED, in DDGI_SampleVolume's units
        //   .a   = resolvedFraction in [0,1], that solid angle's share of the hemisphere
        // so the two tiers own DISJOINT solid angles and the far field supplies the remainder.
        // .a is NOT a confidence: it integrates many rays, so it is spatially smooth and survives
        // denoising. The traced specular fold lerps two estimates of the SAME quantity by a per-ray
        // confidence, a shape that produces salt-and-pepper artifacts (REBLUR never filters alpha). Do
        // not "unify" the two folds.
        // Keep three properties:
        //  1. ssgiCov == 0 reproduces the SSGI-free frame bit-identically (the kill switch).
        //  2. SSGI shares ddgiDiffuseScale and gets NO second brightness knob: two scales turn every
        //     ssgiCov gradient into a visible band. ssgiIntensity is a 0..1 fade for A/B and debug.
        //  3. ssgiNear is NOT multiplied by `ao`: the screen march IS the near-field occlusion. The far
        //     half keeps ddgiAO, so ddgiAONearFieldStrength retires by itself wherever SSGI covers.
        float3 ssgiNear = float3(0, 0, 0);
        float  ssgiCov  = 0.0;
        if (!BindlessNone(ssgiResultIdx) || !BindlessNone(gRestirGiIdx))
        {
            Texture2D<float4> gSSGI = BindlessTex2D<float4>(!BindlessNone(gRestirGiIdx) ? gRestirGiIdx : ssgiResultIdx);
            const float4 ssgi = gSSGI.Load(int3(int2(i.pos.xy), 0));
            ssgiCov  = saturate(ssgi.a) * (!BindlessNone(gRestirGiIdx) ? 1.0 : saturate(ssgiIntensity));
            ssgiNear = ssgi.rgb * ddgiDiffuseScale * (!BindlessNone(gRestirGiIdx) ? 1.0 : saturate(ssgiIntensity));
        }
        // ReSTIR GI's tier of the indirect-specular fallback below: its traced coverage (the
        // cosine-weighted share of this pixel's hemisphere whose rays hit geometry) is the sky visibility
        // and its hit radiance the rough band - the two jobs DDGI's probes do without ReSTIR GI. Without
        // it nothing occludes the floor's sky reflection when DDGI is off. Coverage 0 carries no
        // occlusion information (no estimate, or open sky): DDGI keeps the pixel. SSGI's coverage is
        // screen-limited (an uncovered ray is not sky), so SSGI never takes this tier.
        bool restirGiSpec = !BindlessNone(gRestirGiIdx) && ssgiCov > 0.0;
#if NPR_PASS
        // Toon receivers opt out of the SSGI / ReSTIR GI near field; the (1 - ssgiCov) remainder hands
        // their whole hemisphere to the isotropic far field above:
        //  1. SSGI integrates a cosine hemisphere around the VERTEX NORMAL, the one ambient tier the
        //     isotropic taps cannot de-facet: at close range it owns ~94% of a face's ambient and
        //     repaints the triangle-crease steps the ramp hides.
        //  2. Its rays read PREVIOUS-FRAME scene colour, feeding the character's stylized shading back
        //     into its own ambient - a hue feedback loop.
        // PBR pixels are untouched.
        ssgiNear = float3(0, 0, 0); ssgiCov = 0.0; restirGiSpec = false;
#endif
        float3 iblDiffuse   = ssgiNear + farDiffuse * (1.0 - ssgiCov);
#if NPR_PASS
        // PT-lit toon: the traced sphere-mean radiance IS what the isotropic DDGI / sky taps estimate,
        // with traced visibility (no DDGI AO split); the texture AO still applies.
        if (nprPtLive) iblDiffuse = nprPt.rgb * ao;
#endif
        // ANISOTROPIC environment reflection. The indirect-specular chain uses one reflection vector and
        // one isotropic roughness: the split-sum LUT (indirect_specular.hlsli) has no anisotropy axis,
        // and adding one means re-baking asset/IBL/BRDF_LUT.itex and changing every consumer. Bending the
        // normal the reflection is taken about buys the stretched reflection (brushed metal smearing the
        // sky along the grain). SM_AnisotropicBentNormal returns N exactly at anisotropy 0, so it is inert
        // for every other model. The traced tier is a screen-space fetch at this pixel, not directional,
        // so tracedSpecularDelta and tracedDeltaScale stay exact.
        const float3 Nrefl  = SurfaceIsAnisotropic(sm)
            ? SM_AnisotropicBentNormal(N, V, sm.tangentWS, sm.bitangentWS,
                                       sm.anisotropy, roughness)
            : N;
        float3 R            = reflect(-V, Nrefl);
        // max() guard: this block also runs with iblRadianceMips == 0 (DDGI/probes only, no sky), where
        // the uint `0u - 1` would wrap to 4.29e9. Clamped, it samples mip 0 of the black fallback cube:
        // the intended zero.
        float  mip          = roughness * float(max(iblRadianceMips, 1u) - 1u);
        // No probe mip here: SampleProbe picks each probe's mip (its own mipCount, ProbeRoughnessToMip)
        // and applies the proxy parallax + Lagarde correction.

        // ---- Indirect specular fallback: sky -> DDGI rough -> probes ----
        // All sources stay in pre-Fresnel radiance space; the EnvBRDF fold happens once, after the
        // traced/fallback blend.
        float3 ddgiSpecular  = float3(0.0, 0.0, 0.0);
        float  ddgiSpecCoverage = 0.0;
        // ReSTIR GI's tier replaces both DDGI samples (restirGiSpec). Below ROUGH_START the roughBlend
        // smoothstep is 0, so the R-direction traversal is skipped and the spec samples stay zero - the
        // same result. (The sky visibility comes from the diffuse block's moments.)
        if (ddgiEnabled != 0 && ddgiVolumeCount > 0 && !restirGiSpec
            && roughness > INDIRECT_SPECULAR_DDGI_ROUGH_START)
        {
            // The same volume buffer as the indirect-diffuse block, opened inside the same gate.
            StructuredBuffer<DDGIVolumeGPU> ddgiVolumesSpec =
                BindlessSBuf<DDGIVolumeGPU>(ddgiVolumeBufIdx);

            // The SAME arbitration as the indirect-diffuse block: DDGI_PickVolumes is a pure function of
            // worldPos and the volume buffer, so both blocks resolve to the same winner and a pixel's diffuse
            // and rough-specular fallback come from the same volume.
            DDGIVolumePick pickSpec = DDGI_PickVolumes(worldPos, ddgiVolumesSpec);

            float4 bestSpec   = float4(0.0, 0.0, 0.0, 0.0);
            float4 secondSpec = float4(0.0, 0.0, 0.0, 0.0);

            [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
            {
                if (!DDGI_IsPicked(pickSpec, vi)) continue;
                DDGIVolumeGPU vol = ddgiVolumesSpec[vi];
                const bool isBest = (vi == (uint)pickSpec.bestSlot);

                // NonUniformResourceIndex required (per-pixel vi). Irradiance is evaluated along R, not N, on
                // purpose: a low-frequency rough-specular stand-in, not a second diffuse contribution.
                StructuredBuffer<DDGIProbeData> volProbeData =
                    ResourceDescriptorHeap[NonUniformResourceIndex(vol.probeDataBaseIdx + vi)];
                Texture2D<float4> volIrradiance =
                    ResourceDescriptorHeap[NonUniformResourceIndex(vol.irradianceBaseIdx + vi)];
                Texture2D<float2> volDepth =
                    ResourceDescriptorHeap[NonUniformResourceIndex(vol.depthBaseIdx + vi)];
                const float4 ds = DDGI_SampleVolume(
                    worldPos, R, V, vol,
                    volIrradiance, volDepth,
                    volProbeData, gIBLSampler);
                if (isBest) bestSpec = ds; else secondSpec = ds;
            }

            const float4 specResolved =
                DDGI_CombineWinners(bestSpec, secondSpec, pickSpec);
            if (specResolved.a > 1e-4)
            {
                ddgiSpecular     = specResolved.rgb * ddgiDiffuseScale;
                ddgiSpecCoverage = specResolved.a;
            }
        }

        // The reflection's sky visibility: DDGI's directional visibility ALONG R, its lobe widened by
        // roughness; ReSTIR GI's tier uses its coverage instead. Where DDGI's visibility owns the
        // occlusion it also owns the ground below the horizon, so the fixed horizon guess remains only
        // where no volume covers (skyDirCov 0).
        const float skyDirCov = restirGiSpec ? 0.0 : saturate(skyCov);
        float skyVisibility = lerp(1.0, DDGI_SkyVisibility(skyMoments, R, DDGI_SkyLobeK(roughness)), skyDirCov);
        if (restirGiSpec) skyVisibility = 1.0 - ssgiCov;
        const float3 skySpecular = gRadiance.SampleLevel(gIBLSampler, R, mip).rgb;
        const float horizon = lerp(IndirectSpecular_HorizonVisibility(R), 1.0, skyDirCov);
        const float3 skyTerm = skySpecular * (iblStrength * skyVisibility * horizon);
        const float roughBlend = smoothstep(
            INDIRECT_SPECULAR_DDGI_ROUGH_START,
            INDIRECT_SPECULAR_DDGI_ROUGH_END,
            roughness) * ddgiSpecCoverage;
        float3 distantFallback = lerp(skyTerm, ddgiSpecular, roughBlend);
        // ReSTIR GI's rough band: a wide lobe sees geometry with the coverage's
        // probability and the occluded sky otherwise - lerp(skyTerm, ssgiNear
        // + skyTerm, band) - the same band DDGI hands over in (its irradiance
        // along R holds sky and geometry together).
        if (restirGiSpec)
            distantFallback = skyTerm + smoothstep(INDIRECT_SPECULAR_DDGI_ROUGH_START,
                                                   INDIRECT_SPECULAR_DDGI_ROUGH_END, roughness) * ssgiNear;

        // Probe alpha is filtered geometry coverage; sky pixels in the bake have alpha = 0 and fall
        // through to distantFallback without parallax. Probes come from the per-cluster list
        // ClusterCullProbes.cs emits: an empty cluster gives the sky path 100% of the weight.
        // reflectionProbeCount > 0 gates the cluster lookup, so scenes with probes but no cluster pass
        // wired skip cleanly.
        IndirectSpecularProbeAccum probeAccum = IndirectSpecular_MakeProbeAccum();

        if (reflectionProbeCount > 0)
        {
            // Every index comes from a constant buffer, so it is wave-uniform: no NonUniformResourceIndex().
            // The element types must match the SRVs C++ creates byte for byte (strides 8, 4 and 192); Tier 3
            // reports nothing on a mismatch, which is also why a resource not produced yet is handed over as
            // a type-correct NULL structured buffer, not a filler texture descriptor.
            StructuredBuffer<ProbeGridEntry> probeGrid =
                BindlessSBuf<ProbeGridEntry>(reflProbeGridIdx);
            StructuredBuffer<uint> probeIndexList =
                BindlessSBuf<uint>(reflProbeIndexIdx);
            // ReflectionProbeManager's 3-slot UPLOAD ring; the renderer resolves the current frame slot's SRV
            // into reflProbeBufIdx every frame. Stride 192 == sizeof(ReflectionProbe).
            StructuredBuffer<ReflectionProbe> probeBuffer =
                BindlessSBuf<ReflectionProbe>(reflProbeBufIdx);

            float  probeViewZ  = abs(mul(float4(worldPos, 1.0), viewMatrix).z);
            uint3  probeCoord  = GetClusterCoord(i.uv, probeViewZ, clusterNearZ, clusterFarZ);
            uint   probeKey    = ClusterIndex(probeCoord);
            uint   probeOffset = probeGrid[probeKey].offset;
            uint   probeLocal  = probeGrid[probeKey].count;

            for (uint pli = 0; pli < probeLocal && probeAccum.coverage < 0.999; ++pli)
            {
                uint pi = probeIndexList[probeOffset + pli];
                ReflectionProbe probe = probeBuffer[pi];
                float w = ComputeProbeWeight(worldPos, probe);
                if (w <= 1e-3) continue;

                float4 sample4 = SampleProbe(probe, worldPos, R, roughness, gIBLSampler);
                // The capture scaled by local / capture-point far-field irradiance, darkening only, so an outdoor
                // capture reflected indoors does not carry the outdoors' brightness. Without DDGI both points see
                // the same unoccluded sky (ratio 1): skipped. The flag is the renderer's (-probe_norm_off clears
                // it).
                const float probeNorm = (ddgiEnabled != 0 && ddgiVolumeCount > 0
                                         && (probe.flags & REFLECTION_PROBE_FLAG_NORMALIZE) != 0u)
                    ? ProbeNorm_Scale(localFarIrr, probe.capturePosWS, N) : 1.0;
                IndirectSpecular_AddProbe(probeAccum, sample4, w, probe.intensity * probeNorm);
            }
        }

        float3 fallbackRadiance = IndirectSpecular_ResolveProbeFallback(
            probeAccum, distantFallback);
        const float specOcc = IndirectSpecular_SpecularOcclusion(ao, NdotV, roughness);
        fallbackRadiance *= specOcc;

        // Traced fold: final = refl * conf + fallback * (1 - conf). The traced texture carries the WHOLE
        // traced reflection (screen-space Hi-Z tier + RT-ray tier: one lobe, one denoiser, made in
        // RTReflectionPass) and is SAME-frame; the FullFold fallback's SSR output is one frame late, colour
        // and confidence together. Do not blend a second, frame-latent confidence field in here: two
        // temporally misaligned confidences leave trailing bright/dark bands on motion. On water frames
        // SSRComposite.cs adds the standalone SSR to HDR scaled by (1 - this confidence), so a pixel folded
        // here is not counted twice. Folding refl.rgb in specRadiance space keeps it on the probe/sky tiers'
        // Fresnel / envBRDF / AO / NPR path.
        const float  rtConf  = saturate(tracedReflection.a);
        const float3 specRadiance = lerp(
            fallbackRadiance, tracedReflection.rgb, rtConf);

        float3 Fibl    = FresnelSchlickRoughness(NdotV, F0, roughness);
        float2 envBRDF = IndirectSpecular_SampleEnvBRDF(
            gBRDFLUT, gIBLSampler, NdotV, roughness);
        // The split-sum BRDF LUT contains a grazing/bias term, so F0 == 0 does
        // NOT make its result exactly zero. For NPR, reflectance == 0 is the
        // authored "no PBR specular" switch (Endfield faces ship this way), and
        // it must gate every reflection source, not only direct GGX.
        float nprIndirectSpecularGate = 1.0;
#if NPR_PASS
        nprIndirectSpecularGate = (reflectance > 1e-4) ? 1.0 : 0.0;
#endif
        float3 specIBL = IndirectSpecular_ApplyEnvBRDF(
            specRadiance, F0, envBRDF) * nprIndirectSpecularGate;
        float3 fallbackSpecIBL = IndirectSpecular_ApplyEnvBRDF(
            fallbackRadiance, F0, envBRDF) * nprIndirectSpecularGate;

        // ---- Clear coat: the coat's ENVIRONMENT reflection ----
        // Car paint is a rough base under a mirror-smooth film: a SHARP environment reflection over a
        // blurry one, which no single lobe can produce. The coat gets its own mip (its own roughness), its
        // own split-sum fetch at the dielectric F0 = 0.04, and attenuates the base by its Fresnel as in the
        // direct lobe. Applied to BOTH specIBL and fallbackSpecIBL on purpose: tracedSpecularDelta is
        // their difference, and one affine map (v -> v * atten + coat) keeps it exact (atten * the
        // original delta); adding the coat to specIBL alone would put the whole coat into the delta and
        // corrupt Target0. coatDiffuseAtten lives outside the branch so it can also attenuate the diffuse
        // IBL (kDibl is declared after the traced fold).
        float coatDiffuseAtten = 1.0;
        float3 coatSkySpec     = float3(0, 0, 0);   // the coat's reflection: all sky (the IBL Only view)
        float3 coatFlatSpec    = float3(0, 0, 0);   // ... and of the flat ambient (below)
        [branch]
        if (SurfaceIsClearCoat(sm) && sm.clearCoat > 0.0)
        {
            const float coatRough = max(sm.clearCoatRoughness, 0.045);
            const float coatMip   = coatRough * float(max(iblRadianceMips, 1u) - 1u);
            // The coat is a smooth film on the SHADING normal: it reflects about the unbent R, never the
            // anisotropic bent normal (that describes the base's microfacets). Its sky is occluded like the
            // base's (DDGI's visibility along its own direction, the horizon guess only where no volume
            // covers, ReSTIR GI's coverage on that tier) and, as forward's coat, by the specular occlusion
            // at its own roughness.
            const float3 coatR      = reflect(-V, N);
            const float  coatSkyVis = restirGiSpec ? skyVisibility
                : lerp(1.0, DDGI_SkyVisibility(skyMoments, coatR, DDGI_SkyLobeK(coatRough)), skyDirCov);
            const float  coatHorizon = lerp(IndirectSpecular_HorizonVisibility(coatR), 1.0, skyDirCov);
            const float3 coatRadiance =
                gRadiance.SampleLevel(gIBLSampler, coatR, coatMip).rgb
                * (iblStrength * coatSkyVis * coatHorizon
                   * IndirectSpecular_SpecularOcclusion(ao, NdotV, coatRough));
            const float2 coatEnvBRDF = IndirectSpecular_SampleEnvBRDF(
                gBRDFLUT, gIBLSampler, NdotV, coatRough);
            const float3 coatSpec = IndirectSpecular_ApplyEnvBRDF(
                coatRadiance, float3(0.04, 0.04, 0.04), coatEnvBRDF)
                * sm.clearCoat * nprIndirectSpecularGate;

            const float coatFcIbl =
                (0.04 + 0.96 * pow(saturate(1.0 - NdotV), 5.0)) * sm.clearCoat;
            const float coatAtten = 1.0 - coatFcIbl;

            specIBL         = specIBL         * coatAtten + coatSpec;
            fallbackSpecIBL = fallbackSpecIBL * coatAtten + coatSpec;
            // The coat also shades what reaches the base diffusely.
            coatDiffuseAtten = coatAtten;
            coatSkySpec      = coatSpec;
            coatFlatSpec     = IndirectSpecular_ApplyEnvBRDF(
                flatAmbientL, float3(0.04, 0.04, 0.04), coatEnvBRDF) * sm.clearCoat * nprIndirectSpecularGate;
        }

        tracedSpecularDelta = specIBL - fallbackSpecIBL;

        float3 kDibl     = (1.0 - Fibl) * (1.0 - metalness) * coatDiffuseAtten;
#if NPR_PASS
        // The toon ramp is the authored form model. A second NdotV-dependent
        // energy-conservation darkening on ambient diffuse reveals the mesh's
        // interpolated normal planes whenever direct diffuse is reduced. Keep
        // metal energy conservation, but make toon diffuse view-stable;
        // specular remains independently controlled by the gates above.
        kDibl = (1.0 - metalness).xxx * coatDiffuseAtten;
#endif
        // Diffuse IBL: AO, the per-source scales and iblStrength (sky part) are already in iblDiffuse.
        // Specular IBL: specular occlusion was applied to the fallback only, before the traced blend; the
        // traced tier carries its own visibility and is not AO-darkened twice.
        float3 diffIBL   = kDibl * albedo * iblDiffuse;
        // The SSGI-attributable part of diffIBL, in the post-BRDF space Target0 is subtracted in.
        // Derived, not recomputed: iblDiffuse = ssgiNear + farDiffuse * (1 - ssgiCov), so against an
        // SSGI-free frame (iblDiffuse == farDiffuse) the delta is ssgiNear - farDiffuse * ssgiCov. Zero
        // when ReSTIR GI supplies the near field.
        tracedDiffuseDelta = !BindlessNone(gRestirGiIdx) ? 0 : kDibl * albedo * (ssgiNear - farDiffuse * ssgiCov);

        // The sky views. Diffuse: the sky term's share of the far field times the
        // part of the hemisphere SSGI / ReSTIR GI left to it. Specular: the sky
        // term inside distantFallback (roughBlend is 0 on ReSTIR GI's tier, whose
        // DDGI samples are skipped), what the probes left of it, the specular
        // occlusion and what the traced tier left; the coat's reflection is all
        // sky and attenuates the base like everywhere else.
        [branch] if (viewMode == VIEW_MODE_SKY_VISIBILITY || viewMode == VIEW_MODE_IBL_ONLY)
        {
            const float skyDiffOwn = saturate(1.0 - ddgiCoverage) * (1.0 - ssgiCov);
            dbgSkyVisibility = float3(skyDiffOwn * skyAO * skyVisDiff, skyVisibility * horizon * specOcc,
                                      1.0 - skyDiffOwn);
            const float3 skySpecRad = skyTerm * (1.0 - roughBlend)
                                    * saturate(1.0 - probeAccum.coverage) * specOcc * (1.0 - rtConf);
            const float3 skySpecIBL = IndirectSpecular_ApplyEnvBRDF(skySpecRad, F0, envBRDF)
                                    * nprIndirectSpecularGate * coatDiffuseAtten + coatSkySpec;
            dbgIblOnly = kDibl * albedo * skyDiffPart * (1.0 - ssgiCov) + skySpecIBL;
        }

        // ---- Flat environment ambient: WorldEnvironment None's fill ----
        // A uniform environment of radiance flatAmbientL, weighted like every other ambient: its diffuse
        // through kDibl (a metal takes none, a dielectric loses its Fresnel share), its reflection through
        // the split-sum (a uniform environment reflects L * (F0 * A + B), which keeps a metal lit) with
        // the fallback tier's specular occlusion, plus the coat's reflection of it. Added on top of the
        // partition and unseen by DDGI / RT / the path tracer ON PURPOSE: it is None's non-physical fill.
        // FlatColor's ambient is a uniform SKY instead: Renderer_IBL.cpp binds it as the sky cubes and
        // zeroes this term.
        const float3 flatDiff = kDibl * albedo * flatAmbientL * ao;
        const float3 flatSpec = IndirectSpecular_ApplyEnvBRDF(flatAmbientL * specOcc, F0, envBRDF)
                              * nprIndirectSpecularGate * coatDiffuseAtten + coatFlatSpec;

        IndirectLighting ind;
        ind.diffuse  = diffIBL + flatDiff;
        ind.specular = specIBL + flatSpec;
        AccumulateIndirect(acc.agg, ind);

#if NPR_PASS
        // IBL diffuse stays a VECTOR (it keeps the environment colour) and joins albedo at the composite,
        // bypassing the ramp: ambient light is omnidirectional, not N.L-shaped. iblStrength is already in
        // iblDiffuse (sky part) and specIBL. The flat ambient rides along (AO folded in here).
        acc.nprAmbientColor  += kDibl * (iblDiffuse + flatAmbientL * ao);
        acc.nprIndirSpec     += specIBL + flatSpec;   // toon block: never ramp-boost IBL specular
#endif
    }

    // Emissive is not in this output: the GBuffer pass wrote it to HdrSceneColor (RT5), and the PSO's
    // additive blend adds this lit result to it, so emissive never passes through the BRDF.
    // The taxonomy's one collapse point: the direct and indirect buckets are summed here, once.
    // Subsurface has no block of its own: it is evaluated per light (the sun block and
    // AccumClusteredLight) through EvalSurfaceTransmission into direct.transmission, where
    // AccumulateDirect gates it with that light's own shadow; wrapped diffuse rides SurfaceDiffuseNoL
    // in the same two places. Its parameters are named MaterialGPUData fields resolved once into `sm`.

    float3 color = acc.agg.direct.diffuse
                 + acc.agg.direct.specular
                 + acc.agg.direct.transmission
                 + acc.agg.indirect.diffuse
                 + acc.agg.indirect.specular;

#if NPR_PASS
    // ---- The toon model: rebuild `color` from the NPR accumulators ----
    // Every NPR_PASS pixel is a Toon pixel and takes this one model. Its ramp is the material's own
    // ramp texture (`toonRampIdx`, the material's toonRampTex, one of its CUSTOM textures; the engine
    // RAMPMAP slot carries no signal - every material is constructed with a default ramp path) or,
    // without one, its two authored ramp colours: the same fallback as the forward path and the
    // ray-traced hit shader. nprMat / nprExtra come from the toon context, read before the clustered
    // loop.
    float nprRimStrength     = nprMat.nprRimStrength;
    float nprBrightnessClamp = nprMat.nprBrightnessClamp;

    // How much AO is allowed to touch DIRECT light: nprDirectOcclusion, the one
    // named knob for it. AO is an INDIRECT visibility estimate; the shadow map
    // already answered the direct question, so applying it at full strength to
    // direct light double-counts and crushes cavities.
    float nprDirectOcc = lerp(1.0 - saturate(nprMat.nprDirectOcclusion), 1.0, ao);

    // NPR specular ramp: stylize the SUN's direct specular into a stepped anime highlight; opt-in via
    // nprSpecRampBlend (0 = physical). It works on nprSunSpecUngated alone - a reflection probe or a
    // traced reflection must not trip the glint or tint it - and runs before the toon block reads it.
    {
        float specBlend = saturate(nprMat.nprSpecRampBlend);
        if (specBlend > 0.0)
        {
            float specSmooth = max(nprMat.nprSpecSmooth, 0.001);
            // Clamp the lower edge to >= 0 so a ZERO input never produces a nonzero step: with
            // specThr < specSmooth, smoothstep(-0.02, 0.02, 0) = 0.25 - a glint on a pixel with no specular.
            float specThr    = max(nprMat.nprSpecThreshold, specSmooth);
            float specStr    = nprMat.nprSpecStrength;

            // Hue and gate both come from the SUN, so a punctual light cannot supply a colour the sun's
            // geometry switches on; punctual specular stays physical. The coordinate is
            // Luminance(nprSunSpecUngated), not the bare BRDF nprSunSpecLum: both are visibility-free (a
            // shadowed or dim sun must not slide the glint), but the ungated specular carries lightColor and
            // NdotL - the magnitude nprSpecThreshold's 0.5 default is calibrated against; the bare BRDF kills
            // the glint above roughness ~0.2.
            const float3 sunSpec = acc.nprSunSpecUngated;
            const float  specLum = Luminance(sunSpec);
            const float  stepped = smoothstep(specThr - specSmooth, specThr + specSmooth,
                                              specLum);
            // Preserve highlight hue, replace magnitude with the hard-stepped term.
            const float3 specHue  = (specLum > 1e-5) ? (sunSpec / specLum) : float3(1, 1, 1);
            const float3 stylized = specHue * stepped * specStr;
            // No visibility multiply here, on purpose: nprSunSpecUngated is ungated by contract, and the toon
            // block applies the visibility it is entitled to (its stylized edge).
            acc.nprSunSpecUngated = lerp(sunSpec, stylized, specBlend);
        }
    }

    // ---- The DanbaidongRP-style toon model ----
    // Every Toon pixel: a FACE (SDF marker in extra.a), a material with a toon ramp
    // (toonRampTex >= 0), or one with neither, whose ramp is its two authored colours:
    //   formShadowArea   = face SDF / hair / half-lambert form shadow only
    //   directVisibility = raw CSM / per-object / RT sunVisibility
    //   main diffuse     = albedo * shadowRamp(formShadowArea, row 0)
    //                    * lightColor * directVisibility (NO extra NdotL)
    //   other diffuse    = ramped point/spot + ambient / DDGI
    //   specular         = direct GGX / glint, cast-shadow gated; IBL stays indirect.
    {
        // The main directional's shadow term, through the clustered loop's helper (one terminator
        // implementation for every light):
        //   V2 face  -> the real SDF sweep against THIS light.
        //   V1 face  -> its baked (main-light) terminator, as authored.
        //   non-face -> sharpened half-lambert in DanbaidongRP SigmoidSharp units (e-slope ~34.5 * sharp):
        //               "Shadow Smooth" keeps its direction (bigger = softer), remapped so the default
        //               (0.05) lands on the reference edge.
        float shadowTerm  = (toonIsFace && !toonCtx.faceSDF)
                          ? saturate(nprExtra.r)
                          : NPRToonShadowTerm(toonCtx, N, L);

        // Hair (bangs) shadow on the FACE only, min()'d into the SDF terminator so the fringe takes the
        // same ramp shadow colour as the SDF form shadow. Two sources (HairShadowCasterComponent.mode):
        //   * SCREEN MARK (extra.g, default): the DanbaidongRP CharacterFringe port - HairShadowMarkPass
        //     redraws the bangs with a view-space offset and stamps extra.g on the face pixels it covers.
        //   * SHADOW MAP (LightCB.hairShadowIdx): the fitted bangs-only depth map, opened bindless in
        //     SampleHairShadow.
        // Both are independent of sunVisibility, so a face with Receive Shadow disabled (the usual NPR
        // setup) still gets its bangs shadow. hairShadowStrength scales it.
        if (toonIsFace)
        {
            float hairTerm = (nprExtra.g > 0.5) ? 0.0 : 1.0;     // screen mark
            if (hairShadowParams.x != 0.0)                        // shadow map
            {
                // @vis-linear-REVIEWED — a PHYSICAL query (16-tap SampleCmp)
                // going through SigmoidSharp into the ramp U, kept on purpose: the result is still
                // multiplied by directVisibility at the composite, so it cannot leak sun energy, and the
                // default Screen Mark mode has no sigmoid in the path. A bangs-occluded face pixel resolves
                // to the ramp's authored dark end, not black - the intended look; changing it is an art
                // decision, not a correctness fix.
                float hairPcf = SampleHairShadow(worldPos, N, i.pos.xy);
                hairTerm = min(hairTerm, SigmoidSharp(hairPcf, 0.5, 0.5)); // @vis-linear-REVIEWED
            }
            shadowTerm = min(shadowTerm, lerp(1.0, hairTerm, saturate(hairShadowStrength)));
        }

        // Authored form/self shadow (the ramp: face SDF, hair, half-lambert) stays separate from scene
        // cast-shadow energy (directVisibility, a linear gate). Do NOT sigmoid scene visibility into the
        // ramp U (min(shadowTerm, SigmoidSharp(sunVisibility, ...))): the sigmoid's tail is negligible
        // (SigmoidSharp(0, 0.5, 0.5) = 1.8e-4), but the ramp at U ~ 0, V = TOON_RAMP_V_DIRECT_SHADOW is
        // the authored warm, non-black floor - O(1) sun-coloured light inside a full shadow.
        // This deviates from DanbaidongRP on purpose: PBRToonBase.shader min()s
        // SigmoidSharp(shadowAttenuation) into the ramp U and PBRToonFace.shader min()s SDF + hair + scene
        // shadow into one U, both with no linear gate, so both leak. Do NOT re-sync this block upstream.
        float formShadowArea = lerp(1.0, shadowTerm, saturate(nprMat.nprShadowStrength));
        // @vis-linear — a pure multiplier at both composite sites below, whichever
        // branch is taken.
        //
        // Endpoint proof: ToonVisibility(v, c, w) = smoothstep(saturate(c - w), saturate(c + w),
        // saturate(v)), both bounds in [0, 1], so it is EXACTLY 0 at v == 0 and EXACTLY 1 at v == 1 for
        // every parameter: a fully occluded pixel gets zero direct energy however the edge is tuned
        // (SigmoidSharp(0, 0.5, 0.1) = 0.151 cannot promise that). MAT_FLAG_NPR_TOON_SHADOW_EDGE opts in;
        // without it this is saturate(sunVisibility).
        float directVisibility;
        if (nprMat.materialFlags & MAT_FLAG_NPR_TOON_SHADOW_EDGE)
            directVisibility = ToonVisibility(sunVisibility, 0.5,
                                              saturate(nprMat.nprSceneShadowSmooth));
        else
            directVisibility = saturate(sunVisibility);

        // Direct-only GGX for the toon spec: DanbaidongRP keeps the environment reflection separate
        // (PostEvaluate). The sun's specular takes the SAME directVisibility as the diffuse, so a stylized
        // shadow edge is one edge in both; punctual specular keeps its own per-light visibility.
        // @vis-linear — directVisibility is a plain multiplier (and is itself
        // endpoint-pinned when MAT_FLAG_NPR_TOON_SHADOW_EDGE is set).
        //
        // ---- FACE: direct specular follows the SDF, like diffuse ----
        // specFormGate gates a face's sun specular by the form shadow the diffuse ramp is sampled at.
        // The GGX lobe cannot find the boundary itself: the terminator is an AUTHORED lightmap (the
        // `_SDF` sweep) while the shading normal is the smooth normal blended toward a head SPHERE
        // (EndfieldFace.ps `UseSphereNormal`), so N.L and the SDF disagree by design and an N.L-only
        // direct term leaks across - a full sheen on the shadowed cheek. formShadowArea, not shadowTerm:
        // it carries the hair mark and the material's Shadow Strength, the diffuse ramp's own gate.
        // NON-FACE parts stay at 1.0: their form shadow is a function of N, and the Blender reference
        // (`PBRToonBase`) gates body specular by `saturate(NoL) * castShadows` only.
        // acc.nprPunctualSpec is NOT multiplied: each clustered light was gated by its own terminator
        // (its own SDF sweep side) in the loop; the sun's gate would shade a torch with the sun's yaw.
        float  specFormGate   = toonIsFace ? formShadowArea : 1.0;
        float3 toonDirectSpec = acc.nprSunSpecUngated * directVisibility * specFormGate
                              + acc.nprPunctualSpec;
        // Spec-ramp U = sun BRDF peak proximity (his `saturate(DV)`), NOT the lit/shadowed spec
        // luminance: a dim sun or partial shadow must not slide the glint to the ramp's dark columns.
        float  specRange = saturate(acc.nprSunSpecLum);

        float3 shadowRampCol, specRampCol;
        if (toonRampIdx >= 0)
        {
            Texture2D ramp = g_AllTextures[NonUniformResourceIndex(toonRampIdx)];
            shadowRampCol = ramp.SampleLevel(gSampler, float2(formShadowArea, TOON_RAMP_V_DIRECT_SHADOW), 0).rgb;
            // A 1-D (Nx1) ramp has no spec row: sampling one would return the DIFFUSE ramp and paint an
            // additive coloured glint over everything.
            specRampCol   = toonCtx.rampHasSpecRow
                          ? ramp.SampleLevel(gSampler, float2(specRange, TOON_RAMP_V_DIRECT_SPEC), 0).rgb
                          : float3(0, 0, 0);
        }
        else
        {
            shadowRampCol = lerp(nprMat.nprShadowRampColor, nprMat.nprDiffuseRampColor, formShadowArea);
            specRampCol   = float3(0, 0, 0);   // no ramp -> no extra glint (physical spec kept below)
        }

        // Diffuse = albedo * (1 - metal) (DanbaidongRP ComputeDiffuseColor / PostEvaluate): metals get
        // ~0 diffuse and go dark and reflective. The ramp shapes the shadow; ambient (nprAmbientColor)
        // already carries kDibl = (1 - metal) and AO.
        // DanbaidongRP alignment:
        //   toonLightColor - his `lerp(dirLight.lightColor, _SelfLight, _MainLightColorLerp)` with a
        //     fixed WHITE self-light: raising the lerp decouples the character (the ramp's backlit end
        //     too) from scene light intensity. His materials ship 0.5; the engine default 0 is the full
        //     scene-light response.
        //   directOcc - his `lerp(1 - _DirectOcclusion, 1, AO)`: how much AO darkens DIRECT light (main
        //     and punctual diffuse, all direct specular). The engine default nprDirectOcclusion = 0.1
        //     (Components.h) matches his ~0.1: direct light is nearly unoccluded and cavities darken
        //     through ambient.
        float3 kd = albedo * (1.0 - metalness);
        float3 coupledMainLight = NPRCoupleSceneLight(lightColor * sunTint, toonCtx);
        float3 toonLightColor = lerp(coupledMainLight, float3(1.0, 1.0, 1.0),
                                     saturate(nprMat.nprMainLightLerp));
        // nprDirectOcc above: the one named knob.
        const float directOcc = nprDirectOcc;
        // Main light: the ramp colour at formShadowArea replaces N.L; the Lambert 1/PI stays.
        float3 toonMainDiffuse = kd * shadowRampCol * toonLightColor
                               * (max(nprMat.nprDirectLightScale, 0.0) / PI)
                               * directOcc * directVisibility;
        // Punctual diffuse arrives already ramped per light (AccumClusteredLight) and takes directOcc
        // like every direct term, not raw `ao`: cavities darken through ambient, not by counting AO
        // against direct light twice.
        float3 toonDiffuse = toonMainDiffuse
                           + kd * acc.nprAddColor * directOcc
                           + albedo * acc.nprAmbientColor * max(nprMat.nprAmbientLightScale, 0.0);

        // Spec: DanbaidongRP's ADDITIVE form `F * clamp(specRampCol + DV)` x NdotL * shadowScene *
        // lightColor * directOcclusion, which decomposes into
        //     physical GGX highlight  (F * DV * NdotL * shadow * light: toonDirectSpec)
        //   + stylized ramp glint     (F * specRampCol * NdotL * shadow * light).
        // The glint is ADDED, not multiplied: it survives where the physical lobe is dim (broad hair/skin
        // sheen), which a `* (1 + ramp)` form kills. F is taken at normal incidence:
        // f0 = lerp(0.04, albedo, metalness). The IBL reflection passes through untouched (PostEvaluate).
        float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metalness);
        // The GBuffer's stylized direct-additive, finally gated. @vis-linear —
        // directVisibility is a plain multiplier and the mask carries no light
        // colour of its own, which is exactly what makes this expressible here.
        float3 toonDirectAddCol = toonDirectAdd * toonLightColor * directVisibility
                                * max(nprMat.nprDirectLightScale, 0.0);
        // Same face gate: the glint is a direct sun term driven by N.L, so on a face it must not survive
        // on the SDF's shadow side either. (It is already 0 on Endfield faces, whose 256x1 `_RD` ramps
        // have no spec row; one gate for both direct-spec families keeps them from drifting apart.)
        float3 toonGlint = f0 * specRampCol * saturate(NdotL) * directVisibility
                         * specFormGate
                         * toonLightColor * max(nprMat.nprDirectLightScale, 0.0);
        float3 toonSpec = (toonDirectSpec + toonGlint + toonDirectAddCol) * directOcc
                        + acc.nprIndirSpec;

        color = toonDiffuse
              + toonSpec * max(nprMat.nprSpecularLightScale, 0.0);
        // Target0 bookkeeping: the indirect specular that carries
        // tracedSpecularDelta has just been rescaled, so the delta must be too
        // (tracedDeltaScale is still 1 here). The spec ramp, if it ran, was
        // NON-AFFINE: 0 gives up on an exact subtraction rather than subtract a
        // wrong amount - it leaves the traced tier in Target0, which only
        // disables the self-reflection guard for this pixel.
        tracedDeltaScale = max(nprMat.nprSpecularLightScale, 0.0);
        if (saturate(nprMat.nprSpecRampBlend) > 0.0) tracedDeltaScale = 0.0;

        // Project the toon composite back onto the taxonomy for the debug views: dead stores on the Lit
        // path (`color` above ships and acc.agg is not read again), but they make DirectDiffuse /
        // IndirectDiffuse / ... show this pixel's real terms:
        //   toonMainDiffuse     sun,      DIRECT diffuse   (gated by directVisibility)
        //   kd*nprAddColor      punctual, DIRECT diffuse   (each light gated in the loop)
        //   albedo*nprAmbient   IBL/DDGI, INDIRECT diffuse
        //   toonDirectSpec+glint          DIRECT specular
        //   nprIndirSpec                  INDIRECT specular
        const float nprSpecScale = max(nprMat.nprSpecularLightScale, 0.0);
        acc.agg.direct.diffuse    = toonMainDiffuse + kd * acc.nprAddColor * directOcc;
        acc.agg.indirect.diffuse  = albedo * acc.nprAmbientColor
                                  * max(nprMat.nprAmbientLightScale, 0.0);
        acc.agg.direct.specular   = (toonDirectSpec + toonGlint) * directOcc * nprSpecScale;
        acc.agg.indirect.specular = acc.nprIndirSpec * nprSpecScale;
        acc.agg.direct.transmission = float3(0, 0, 0);   // toon path has none
        // The two visibility quantities, for the PhysicalVisibility / FormShadow views.
        sunVis.form = formShadowArea;

        // ---- DanbaidongRP screen-space depth rim ----
        // Port of GetCharacterDirectRimLightArea + GetRimColor: shift the screen UV sideways along the
        // view-space normal (width shrinks with distance); where the scene depth jumps far behind the
        // pixel, it is a silhouette edge -> rim. Front (lit) / back (shadow) tints follow his defaults
        // (front white * light, back 0.2 * grey), scaled by Rim Strength.
        if (nprMat.nprRimWidth > 0.0 && nprRimStrength > 0.0)
        {
            float3 normalVS   = normalize(mul(N, (float3x3)viewMatrix));
            float3 lightDirVS = normalize(mul(normalize(-lightDir), (float3x3)viewMatrix));
            float  eyeDepth   = max(dot(worldPos - cameraPos, cameraForward), 1e-3);

            float2 extendUV = i.uv;
            extendUV.x += ((normalVS.x > 0.0) ? 1.0 : -1.0)
                        * nprMat.nprRimWidth * 0.0044 / (eyeDepth + 3.0);

            float extZ    = gDepth.SampleLevel(gSampler, extendUV, 0).r;
            float extEye  = dot(ReconstructWorldPos(extendUV, extZ) - cameraPos, cameraForward);
            float rimArea = saturate((extEye - eyeDepth) * 4.0);

            float  NdotLVS     = dot(normalVS, lightDirVS);
            // The rim is DIRECT sun energy and obeys the contract of every direct term: the COUPLED light
            // (chroma/intensity weights + main-light lerp) and nprDirectLightScale, so it does not outshine a
            // character an artist pinned under a strong sun. The defaults (weights 1, scale 1) reproduce the
            // reference: NPRCoupleSceneLight is identity there. The tints are his _DirectRimFrontCol
            // (1,1,1, a=0.5) and _DirectRimBackCol (0.2, a=0.5).
            float3 frontRimCol = lerp(float3(1.0, 1.0, 1.0), toonLightColor,        0.5);
            float3 backRimCol  = lerp(float3(0.2, 0.2, 0.2), 0.2 * toonLightColor,  0.5);
            float3 rimCol = (max(NdotLVS, 0.0) * frontRimCol + max(-NdotLVS, 0.0) * backRimCol)
                          * saturate(kd + 0.3)             // his albedoRimColor = saturate(albedo + 0.3)
                          * saturate(formShadowArea + 0.2)
                          * rimArea;
            // This rim is colored and oriented by the Directional Light, so it
            // is direct energy and must vanish in a fully occluded cast shadow.
            color += rimCol * nprRimStrength * directVisibility
                   * max(nprMat.nprDirectLightScale, 0.0);
        }
    }

    // Brightness floor: the max of the global floor and the per-material one. Both default to 0, so
    // NPR shadows reach true black like PBR (keeping AutoExposure's histogram aligned with the PBR
    // case); artists raise either for a stylized soft-shadow floor.
    // A SMOOTH luminance-domain fill, not a per-channel max(): a max bites each RGB channel at a
    // different place (three C1 creases across the terminator, and an off hue in between). The fill
    // completes the pixel's LUMINANCE to exactly floorLum at lum = 0 and decays hyperbolically above
    // it. The target stays albedo-proportional ON PURPOSE: near-black albedo (black hair/cloth) opts
    // out, so ink-dark materials never grey out - raise the albedo, not the floor, to lift one.
    float minBrightness = max(nprMat.nprMinBrightness, nprMinBrightness);
    float floorLum = 0.0;
    if (minBrightness > 0.0)
    {
        floorLum = Luminance(albedo) * minBrightness;
        if (floorLum > 0.0)
        {
            float curLum = Luminance(color);
            // Adds floorLum^2 / (curLum + floorLum) of luminance: exactly floorLum at curLum = 0, half at
            // curLum == floorLum, negligible once lit.
            color += albedo * minBrightness * (floorLum / (curLum + floorLum));
            // NON-AFFINE where the fill dominates: once the floor carries the pixel, stop tracking the traced
            // delta through it.
            if (curLum < floorLum) tracedDeltaScale = 0.0;
        }
    }

    // Brightness ceiling, RELATIVE and per material (opt-in, MAT_FLAG_NPR_BRIGHTNESS_CLAMP):
    // baseColorLum * nprBrightnessClamp. It caps the LIGHTING GAIN (color / albedo <= k): albedo
    // contrast stays, the response to light flattens. Keep it opt-in: always on at the 1.0 default it
    // pins every NPR pixel to its albedo luminance - the character cannot brighten under a stronger
    // light while PBR neighbours do, and on a dark albedo the ceiling erases the rim and glint added
    // above. Emissive never enters `color` (it arrives via the GBuffer-seeded blend).
    // The absolute per-material / scene-wide ceilings are NPRStabilizePass's, so they see emissive and
    // the face overlay's surface too (on the lit-scene snapshot, exposed-space and soft; VFX/fog
    // composited over the character pass through - NPRStabilize.cs.hlsl).
    {
        const float kNoLimit = 3.402823466e+38f; // FLT_MAX
        float maxLum = kNoLimit;

        if (nprMat.materialFlags & MAT_FLAG_NPR_BRIGHTNESS_CLAMP)
        {
            float baseColorLum = max(Luminance(albedo), 1e-5);
            maxLum = min(maxLum, baseColorLum * nprBrightnessClamp);
            // The floor WINS over this ceiling: both are authored independently, and a clamp below the floor
            // would drag floored shadows back down (floor 0.5 + clamp 0.2 -> shadows at 0.2 x albedo). Both
            // are in Luminance(albedo) units, so the max() is exact.
            maxLum = max(maxLum, floorLum);
        }
        if (maxLum < kNoLimit)
        {
            float curLum = Luminance(color);
            if (curLum > maxLum)
            {
                // This one IS a uniform scale, so it can be tracked exactly.
                const float k = maxLum / curLum;
                color *= k;
                tracedDeltaScale *= k;
            }
        }
    }
#endif

    // ---- Lighting-taxonomy debug views ----
    // Placed BEFORE the aerial-perspective composite on purpose: AP is affine
    // (`color * ap.a + ap.rgb`), so after it every bucket would carry its own `+ ap.rgb` and
    // "DirectOnly + IndirectOnly == Lit" would fail by construction. Here the identity is exact - a
    // real check that nothing escapes the buckets. Caveats:
    //   * Emissive never enters `color` (it arrives through this PSO's additive blend), so it shows in
    //     every one of these views; the identity holds for LIGHTING.
    //   * The NPR permutation rebuilds `color` from the toon composite, so the toon block writes its
    //     terms back into acc.agg for these views (dead stores in Lit).
    [branch]
    if (VIEW_MODE_IS_LIGHTING_DEBUG(viewMode))
    {
        float3 dbg = float3(0, 0, 0);
        if      (viewMode == VIEW_MODE_DIRECT_DIFFUSE)      dbg = acc.agg.direct.diffuse;
        else if (viewMode == VIEW_MODE_DIRECT_SPECULAR)     dbg = acc.agg.direct.specular;
        else if (viewMode == VIEW_MODE_DIRECT_TRANSMISSION) dbg = acc.agg.direct.transmission;
        else if (viewMode == VIEW_MODE_INDIRECT_DIFFUSE)    dbg = acc.agg.indirect.diffuse;
        else if (viewMode == VIEW_MODE_INDIRECT_SPECULAR)   dbg = acc.agg.indirect.specular;
        else if (viewMode == VIEW_MODE_DIRECT_ONLY)
            dbg = acc.agg.direct.diffuse + acc.agg.direct.specular + acc.agg.direct.transmission;
        else if (viewMode == VIEW_MODE_INDIRECT_ONLY)
            dbg = acc.agg.indirect.diffuse + acc.agg.indirect.specular;
        else if (viewMode == VIEW_MODE_SKY_VISIBILITY)
            dbg = dbgSkyVisibility;
        else if (viewMode == VIEW_MODE_IBL_ONLY)
            dbg = dbgIblOnly;
        else if (viewMode == VIEW_MODE_PHYSICAL_VISIBILITY)
            dbg = sunVis.physical.xxx;      // 1 = the sun reaches here
        else /* VIEW_MODE_FORM_SHADOW */
            dbg = sunVis.form.xxx;          // authored surface shading, not occlusion
        return MakeLightingTargets(dbg, dbg);
    }

    // ---- Aerial perspective composite ----
    // Atmospheric scattering + transmittance by view distance, LAST: distance fog is a view-medium
    // effect, independent of the shading model, and the NPR brightness clamps must not crush it.
    // Distance -> LUT slice uses the bake's quadratic mapping, slice_t = sqrt(distKm / maxDistKm),
    // capped so far points clamp to the last slice instead of reading uninitialised data.
    float reflectionDeltaTransmittance = 1.0;
    if (aerialMaxDistKm > 0.0)
    {
        float distKm = length(worldPos - cameraPos) * 0.001; // metres -> km
        distKm = min(distKm, aerialMaxDistKm);               // clamp far range
        float sliceT = saturate(sqrt(distKm / aerialMaxDistKm));
        float4 ap = gAerialPerspective.SampleLevel(gSampler, float3(i.uv, sliceT), 0.0);

        // Guard: before the AP LUT is written (an all-zero sample) ap.a = 0 would paint the scene black;
        // such a sample leaves `color` unchanged.
        if (ap.a > 1e-4 || any(ap.rgb > 1e-4))
        {
            color = color * ap.a + ap.rgb;
            reflectionDeltaTransmittance = ap.a;
        }
    }

    // Both traced deltas come out of the reflection source. tracedDeltaScale and the
    // aerial-perspective transmittance apply to both: whatever affine scaling `color` picked up
    // between the fold and here scaled the deltas with it.
    const float3 reflectionSource = max(
        color - (tracedSpecularDelta + tracedDiffuseDelta)
                * (tracedDeltaScale * reflectionDeltaTransmittance), 0.0);
    return MakeLightingTargets(
        reflectionSource,
        color);
#endif // UNLIT
}
