// custom_surface.hlsli - the custom pixel (surface) template of the GBuffer + Transparent passes;
// the pixel analogue of custom_vertex.hlsli.
//
// PURPOSE
//   Lets a material customize the pixel stage (albedo, normal, roughness/metal/AO, emissive,
//   opacity, NPR scratch) without re-implementing the engine's texture plumbing, TBN/normal-map
//   math, masked clip, geometric specular AA, TAA velocity or the 6-MRT GBuffer encode. The
//   pixel side hooks one function, EvaluateMaterial, that fills a SurfaceData.
//
// HOW TO AUTHOR A CUSTOM SURFACE SHADER
//   Create a `.ps.hlsl` that includes this header and defines exactly one function, then assign
//   it to the material's "Pixel Shader Path" slot (Inspector -> Use Custom Shader).
//
//       #define MATERIAL_HAS_SURFACE          // "I provide EvaluateMaterial"
//       #include "custom_surface.hlsli"
//
//       void EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s)
//       {
//           MaterialGPUData mat = g_Materials[p.materialIndex];
//           s.baseColor = mat.baseColor.rgb * MS_SampleBaseColor(mat, p.uv0).rgb;
//           s.normalWS  = MS_ApplyNormalMap(MS_SampleNormalTS(mat, p.uv0, 1.0), p.tangentToWorld);
//           s.roughness = 0.4; s.metallic = 0.0;
//           // anything you don't set keeps its MS_DefaultSurface() value.
//       }
//
//   The engine supplies `main`: it builds MaterialPixelParams from the vertex interpolants,
//   seeds SurfaceData with defaults, calls the hook, runs masked clip, then encodes the GBuffer
//   (opaque) or forward-shades (transparent).
//
// SHADING MODELS - ANISOTROPIC AND SUBSURFACE
//   The lighting model is a material setting (Inspector -> "Shading Model"), not something the
//   hook selects; for most materials that dropdown plus the two sliders next to it is the whole
//   job - the weak default and every custom hook already produce a correct GBuffer for it.
//
//   Drive them per pixel by writing these SurfaceData fields (inert on a material whose model
//   does not read them, so it is safe to set them unconditionally):
//
//       // Anisotropic - brushed metal, hair, satin
//       s.anisotropy      = -0.8;                     // [-1,1]; see below
//       s.anisotropyDirWS = MS_AnisoDirFromTangentSpace(flow * 2 - 1,
//                                                       p.tangentToWorld);
//
//       // Subsurface - skin, wax, jade
//       s.subsurfaceOpacity = thicknessMap.r;   // 1 = solid, 0 = thin/see-through
//       s.subsurfaceMask    = maskMap.r;        // where scatter is allowed
//
//   `s.anisotropyDirWS` is already seeded with the mesh tangent rotated by the material's
//   Anisotropy Rotation knob, so a hook that ignores it still gets a correct, art-directable
//   result. The subsurface tint and overall strength are per material (there is no GBuffer
//   channel for a per-pixel colour).
//
//   SIGN. Positive anisotropy widens the specular lobe along the tangent (the Unreal
//   convention). Hair and brushed metal, whose tangent runs along the strand, want a negative
//   value (or a 0.25-turn rotation).
//
//   Do not hand-write RT4 (`s.extra`) for either model: the engine packs those channels in
//   MS_EncodeGBuffer with the codec the deferred pass decodes with (shaders/shading_models.hlsli),
//   and a hand-rolled packing cannot stay in sync. `s.extra.b` (hair mask) and `s.extra.a` (face
//   marker / direct-additive) are untouched by that packing and stay yours.
//
// OPTIONAL TUNABLES (same as a custom VS)
//   Declare `cbuffer Foo : register(b8, space0)` and/or unbound `Texture2D`s (t* space3); the
//   material system reflects them into Inspector sliders / texture slots (b8 = per-material
//   custom CBV, t* space3 = custom textures), shared with the custom VS bag.
//
// OPAQUE vs TRANSPARENT
//   The same hook serves both; the material's Blend Mode picks the pass. Opaque -> deferred
//   GBuffer (6 MRTs, deferred lighting + NPR later); any translucent blend -> forward shading
//   (this header does the BRDF + IBL from the SurfaceData and returns one HDR color). Shadow
//   passes run the hook only in the SHADOW_HOOK variant (depth-only coverage, below); otherwise
//   only the custom VS reaches them (custom_vertex.hlsli).
//
// MULTI-UV NOTE
//   The canonical interpolants from GBuffer.vs.hlsl carry uv0..uv3, TBN, vertex color and
//   velocity. Hooks read p.uv2/p.uv3 directly; no custom vertex shader is needed just to sample
//   another UV channel.

#ifndef CUSTOM_SURFACE_HLSLI
#define CUSTOM_SURFACE_HLSLI

// ==== Pass selection: forward (translucent) vs deferred GBuffer (opaque) ====
// The blend-mode permutation flags are emitted as 0/1 defines by MakeDefines; an includer
// (e.g. the default Transparent.ps) may also force SURFACE_FORWARD. Computed before the
// material_surface.hlsli include, whose weak-default hook applies the forward roughness floor
// under MS_IS_FORWARD.
//
// WATER_SURFACE selects a third variant: a forward draw that also stamps the GBuffer
// (normal/surface/velocity) so the screen-space reflection chain can trace off the water the
// same frame. It reuses the SURFACE_FORWARD machinery (LightCB@b2, IBL cubes, probes) rather
// than declaring its own, so the two must not be independent: water implies forward.
#if defined(WATER_SURFACE) && WATER_SURFACE
    #define MS_IS_WATER     1
    #ifndef SURFACE_FORWARD
        #define SURFACE_FORWARD 1
    #endif
#else
    #define MS_IS_WATER     0
#endif
// TRANSLUCENT_PREPASS selects a FOURTH variant: the translucent FRONT-LAYER
// prepass (TranslucentFrontLayerPass). A depth-writing, colour-less draw of
// the forward translucent packets that stamps the NEAREST translucent
// surface's normal / surface / velocity into the TranslucentFront_* targets,
// so RTReflectionPass's second instance can trace reflections FROM the glass
// (its rays start on the pane, with the pane's own normal) and the colour
// pass folds that trace at full strength (MS_FetchFrontLayerReflection).
// Implies forward like water: the hook and the forward roughness floor must
// match the colour draw that consumes the trace. Water wins if both are set
// (it stamps the real GBuffer and is excluded from the fold).
#if defined(TRANSLUCENT_PREPASS) && TRANSLUCENT_PREPASS
    #define MS_IS_FRONTLAYER 1
    #ifndef SURFACE_FORWARD
        #define SURFACE_FORWARD 1
    #endif
#else
    #define MS_IS_FRONTLAYER 0
#endif
// TRANSMISSION_PREPASS selects a FIFTH variant: a Transmissive surface's transmission prepass
// (TransmissionPrepass), colour-less, writing what the background behind it turns into for
// TransmissionApply. Implies forward: the hook runs as in the colour draw.
#if defined(TRANSMISSION_PREPASS) && TRANSMISSION_PREPASS
    #define MS_IS_TRANSMISSION 1
    #ifndef SURFACE_FORWARD
        #define SURFACE_FORWARD 1
    #endif
#else
    #define MS_IS_TRANSMISSION 0
#endif

#if defined(SURFACE_FORWARD) \
 || (defined(ALPHA_BLEND)         && ALPHA_BLEND) \
 || (defined(ADDITIVE_BLEND)      && ADDITIVE_BLEND) \
 || (defined(PREMULTIPLIED_BLEND) && PREMULTIPLIED_BLEND) \
 || (defined(MULTIPLY_BLEND)      && MULTIPLY_BLEND) \
 || (defined(TRANSMISSIVE_BLEND)  && TRANSMISSIVE_BLEND)
    #define MS_IS_FORWARD 1
#else
    #define MS_IS_FORWARD 0
#endif

#include "material_surface.hlsli"
#include "dissolve.hlsli"
#include "dither.hlsli"
#include "alpha_coverage.hlsli"   // the masked coverage rule, shared with Shadow.ps
#include "debug_shade.hlsli"   // empty outside the DEBUG_SHADE permutation

#if defined(DEBUG_SHADE) && DEBUG_SHADE
// The density of the texture MS_SampleBaseColor samples, at @p uv: the deferred path's
// bindless base colour map (none: untextured), the forward path's per-draw g_BaseColor (1 x
// 1: the untextured default).
void MS_DebugShadeDensity(MaterialGPUData mat, float4 svPos, float2 uv)
{
    uint w = 1u, h = 1u;
#if defined(MS_IS_FORWARD) && MS_IS_FORWARD
    g_BaseColor.GetDimensions(w, h);
#else
    const int t = mat.textureHandleIds[0];   // BASECOLORMAP
    if (t >= 0) g_AllTextures[t].GetDimensions(w, h);
#endif
    DebugShade_Density(svPos, DebugShade_Lod(uv, w, h), w * h > 1u);
}
#define DEBUG_SHADE_DENSITY(mat, svPos, uv) MS_DebugShadeDensity(mat, svPos, uv)
#else
#define DEBUG_SHADE_DENSITY(mat, svPos, uv)
#endif

// ==== Canonical vertex interpolants (must match GBuffer.vs.hlsl output) ====
struct MaterialSurfaceVaryings
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;
    float2 uv1       : TEXCOORD3;
    float2 uv2       : TEXCOORD4;
    float2 uv3       : TEXCOORD5;
    float4 curClip   : TEXCOORD1;   // unused by the forward variant (DXC strips it)
    float4 prevClip  : TEXCOORD2;
    // Per-instance slot forwarded by the VS (the PS has no SV_InstanceID); indexes
    // InstanceBuffer for the dissolve params. The InstanceBuffer root SRV is VISIBILITY_ALL.
    nointerpolation uint instanceSlot : INSTSLOT;
};

// ==== Per-instance dissolve (shared by the deferred + forward mains) ====
// Clips dissolved pixels and adds the edge glow into s.emissive. The uniform (per-instance)
// branch keeps the noise sample off the hot path; the mere presence of clip() disables
// early-Z write for these PSOs (test-early/write-late remains - profile before optimizing
// further).
void MS_ApplyDissolve(MaterialSurfaceVaryings i, inout SurfaceData s)
{
    const float4 dparams = InstanceBuffer[i.instanceSlot].dissolveParams;
    [branch]
    if (dparams.x > 0.0 && gDissolveNoiseIdx != 0xFFFFFFFFu)
    {
        const float edge = DissolveClipEdge(g_AllTextures[gDissolveNoiseIdx],
                                            g_LinearWrap, dparams, i.uv);
        const float4 edgeCol = InstanceBuffer[i.instanceSlot].dissolveEdgeColor;
        s.emissive += edgeCol.rgb * (dparams.w * edge);
    }
}

// Build the read-only hook params (TBN + front-face correction + view dir).
MaterialPixelParams MS_MakeParams(MaterialSurfaceVaryings i, bool frontFace)
{
    float3 N  = normalize(i.wn);
    float3 T  = normalize(i.wt);
    float3 BT = normalize(i.wbt);
    // DOUBLE_SIDED: flip the geometry normal + bitangent so they face the camera.
    if (!frontFace) { N = -N; BT = -BT; }

    MaterialPixelParams p;
    p.worldPos       = i.worldPos;
    p.geoNormalWS    = N;
    p.tangentWS      = T;
    p.bitangentWS    = BT;
    p.tangentToWorld = float3x3(T, BT, N);   // rows = T, BT, N
    // Material UV transform (Inspector "UV Transform" -> Tiling / Offset), applied once here
    // so the hook, the weak default, the ALPHA_TEST block and every custom material that
    // reads p.uv0 tile identically. i.uv stays raw for lookups that are not material
    // textures (the dissolve noise has its own noiseScale). uv1 is a second UV set and is
    // deliberately left untouched.
    p.uv0            = MaterialUV(g_Materials[materialIndex], i.uv);
    p.uv1            = i.uv1;
    p.uv2            = i.uv2;
    p.uv3            = i.uv3;
    p.vertexColor    = float4(i.col, 1.0);
    p.viewDirWS      = normalize(gCameraPosWS.xyz - i.worldPos);
    p.time           = gTimeParams.x;        // PerViewCB time tail (animated effects)
    // Object axes in world space. Transformed with the SAME mul(v, world) the
    // vertex stage uses for position, so it cannot disagree with the vertex
    // transform whatever the matrix packing is; w = 0 drops the translation.
    {
        const float4x4 wm = InstanceBuffer[i.instanceSlot].world;
        const float3 ax = mul(float4(1, 0, 0, 0), wm).xyz;
        const float3 ay = mul(float4(0, 1, 0, 0), wm).xyz;
        const float3 az = mul(float4(0, 0, 1, 0), wm).xyz;
        p.objectScale = float3(length(ax), length(ay), length(az));
        p.objectAxes  = float3x3(normalize(ax), normalize(ay), normalize(az));
        p.objectOrigin = mul(float4(0, 0, 0, 1), wm).xyz;
    }
    p.materialIndex  = materialIndex;
    p.isFrontFace    = frontFace;
    p.svPosition     = i.sv;                 // screen-space addressing (see decl)
    return p;
}

// User hook (or the weak default in material_surface.hlsli). Forward-declared so
// `main` can call it; the user definition is resolved later in the same TU.
void EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s);

// ==== Forward (translucent) variant: the engine does the BRDF + IBL from SurfaceData ====
#if MS_IS_FORWARD

#define LIGHT_CB_REGISTER b2
#include "light_cb.hlsli"              // lightDir/lightColor/cameraPos/IBL params
#include "view_mode_common.hlsli"      // VIEW_MODE_* + WIREFRAME_COLOR
#include "sky_sh.hlsli"                // gSkySH + EvalSH2
// Probe struct + helpers only: REFL_PROBE_BUFFER_REGISTER is deliberately left undefined,
// which selects the header's bindless path; the buffer is opened from
// LightCB.reflProbeBufIdx inside the IBL gate below. This include sits inside MS_IS_FORWARD,
// so the deferred (GBuffer) variant of a custom surface shader never sees it.
#include "reflection_probe.hlsli"
#include "brdf.hlsli"
#include "indirect_specular.hlsli"
#include "core/sampling.hlsli"          // SampleGGXVNDF, for the dielectric header
#include "core/bsdf_dielectric.hlsli"   // the Transmissive surface's IOR, thin-slab and absorption terms

// The F0 a Transmissive surface reflects with: its IOR's, both faces' for a thin wall, so the
// reflected and the transmitted light (TransmissionApply) come from one interface.
float MS_TransmissiveF0(MaterialGPUData mat, SurfaceData s)
{
    const float f0 = DielectricIorToF0(max(mat.ior, 1.0));
    return (s.thickness <= 0.0) ? ThinSlabReflectance(f0) : f0;
}

// The same as SurfaceData.reflectance, which encodes F0 as 0.16 reflectance^2.
float MS_TransmissiveReflectance(MaterialGPUData mat, SurfaceData s)
{
    return sqrt(MS_TransmissiveF0(mat, s) / 0.16);
}

TextureCube       gIrradiance : register(t6, space0);
TextureCube       gRadiance   : register(t7, space0);
Texture2D<float2> gBRDFLUT    : register(t8, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#include "bindless.hlsli"       // typed bindless handles
#define gIBLSampler SmpFixed(SMP_LINEAR_WRAP)

// ==== Sun shadow for the entire forward path ====
// The forward BRDF multiplies direct sun by the CSM shadow (ComputeShadowFactor), so every
// translucent surface, mesh-driven water body and lit VFX material is shadowed like the
// deferred surfaces. The declarations live here, not in water_common.hlsli, so every forward
// permutation gets them; do not declare them there too (the second declaration collides).
//
// Root signature cost: zero. Gfx::ShadowSRV covers t9..t11 space0 in the shared graphics
// root signature (RootSignatureLayout.h), and the comparison sampler is a fixed sampler-heap
// slot.
Texture2DArray<float>  gShadowCascades : register(t9, space0);
#define gShadowSampler SmpCmp(SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE)

// PER_OBJECT_SHADOW: water only. Read this before "simplifying" it.
//
// The per-object map holds exactly one thing: the tagged character's own casters (bangs ->
// face, head -> body, body -> ground), rendered at high resolution so a character
// self-shadows crisply. The deferred path swaps a face pixel to
// ComputeShadowFactorExternalOnly so the character's own bangs never project a shadow-map
// shadow onto its face: the face's self-shadow is the SDF terminator, the bangs' is the
// artistic hair term.
//
// Do not enable it for every forward material: a character's translucent parts (eyeshadow,
// eye-blend, lash and transparent-cover layers) sit on the face and would receive the
// character's own hard per-object shadow while the opaque face underneath does not - a hard
// aliased edge on top of an SDF terminator that disagrees with it. Additive parts (glow,
// magic, tracers) would darken in shade too.
//
// Water is where it belongs: a water plane is not part of the character, and while the
// per-object map is active ShadowPass excludes that character from the cascades, so without
// it a character standing on water casts nothing onto it. Hence on for MS_IS_WATER, off for
// the ordinary translucent path, which then matches what the face gets (cascades = the
// external channel).
//
// RT_SHADOW_MASK stays off everywhere here: the mask is traced against the opaque depth
// buffer, so on a transparent surface it answers for whatever is behind the glass, not for
// the glass (MS_ForwardRTShadowMask folds it only by opaque-surface proximity).
#if MS_IS_WATER
    #define PER_OBJECT_SHADOW
#endif
#include "shadow.hlsli"
// Forward parity with Lighting.ps: besides the CSM term on the two direct sun products, the
// forward BRDF folds DDGI, DDGI sky visibility, screen AO and the RT sun mask with the same
// maths Lighting.ps uses, so indoors a translucent surface goes dark like the deferred
// surfaces around it instead of being lit as if under an open sky. The two screen-space
// terms (RT mask, AO) belong to the opaque surface at the pixel and are proximity-gated
// (MS_ForwardOpaqueProximity).
#include "cloud_shadow.hlsli"    // SampleCloudShadow - bindless via LightCB
// DDGI_PickVolumes / DDGI_SampleVolume / DDGI_SampleVolumeSkyMoments: every resource is a
// parameter, opened bindless from LightCB.
#include "DDGISampling.hlsli"
#include "probe_normalization.hlsli"   // probe brightness scale; needs gIrradiance / gIBLSampler above

// ==== Clustered point / spot lights ====
// So glass, alpha-blended posters and every other translucent surface receive lamps: the
// buffers and registers Lighting.ps uses (t13/t14/t15 and the spot-shadow views at t22, all
// in the shared graphics root signature, bound by TransparentPass), with the same shadows
// (punctual_shadow.hlsli). Water declares the same buffers in water_common.hlsli for its
// specular-only loop (WaterLocalLights), so only the ordinary forward permutations declare
// them here.
#if !MS_IS_WATER
#include "cluster_common.hlsli"
#include "light_eval.hlsli"      // the single punctual-falloff authority
StructuredBuffer<GPULight>       gLights         : register(t13, space0);
StructuredBuffer<uint>           gLightIndexList : register(t14, space0);
StructuredBuffer<LightGridEntry> gLightGrid      : register(t15, space0);
#include "punctual_shadow.hlsli" // SampleSpotShadow / SamplePointShadow
#endif

// ==== Traced-reflection reuse for translucent surfaces ====
// RTReflectionPass traces one reflection per screen pixel, about the deferred GBuffer
// surface - for a transparent pixel that is the opaque surface behind it. Nothing traces
// about the transparent surface itself (it is not in the GBuffer), so this reuses that
// pixel's traced result, but only where the two surfaces are (a) near-coplanar and (b) close
// together: the regime in which the traced ray is the ray this surface wants (same
// direction, origin displaced a little). A pane at an angle to the wall behind it, or far in
// front of it, keeps the probe/sky tier. The gate reads the GBuffer normal and the depth
// snapshot, both opened bindless from LightCB (the fold row at offset 1152).
//
//   weight = rtConf                       RTR's own per-pixel confidence: 0 on
//                                         sky, above the traced roughness band,
//                                         and whenever the pass did not run
//          * coplanar(N, Nopaque)         smoothstep on the cosine: full at ~8
//                                         degrees, zero at ~20 degrees
//          * proximity(zGlass / zOpaque)  view-depth ratio (linearised with the
//                                         camera planes from LightCB - the
//                                         projection is finite reverse-Z, so
//                                         the raw NDC ratio is not scale-
//                                         relative near the far plane), in
//                                         (0,1]: full above 0.8, zero below 0.5
//          * gFwdReflStrength             features.transparentReflections
//
// Excluded by construction: water (its late SSR chain composites onto the stamped water
// GBuffer - a fold here would double-count the lakebed's traced result, which is what rtConf
// holds at a water pixel) and MULTIPLY_BLEND (its rgb is a factor, not radiance). Unlit
// returns before the BRDF.
//
// Contracts: rtReflectionIdx is read with an ungated Load() - the fallback is a 1x1 black
// texture and an out-of-range Load returns 0 (rtConf 0), the idiom Lighting.ps relies on;
// never Sample() it. The texture is same-frame whenever the unified RTReflectionPass ran
// (RTR, SSR or both); without NRD/DXR the SSR FullFold chain binds last frame's SSR output
// there instead, one frame stale identically for Lighting.ps's opaque fold, so the two tiers
// agree. Every index is a CB scalar, wave-uniform: no NonUniformResourceIndex(). 0 means
// unbound for the two GBuffer indices (heap slot 0 is a real descriptor). The three textures
// and this draw are at render resolution under the same jittered viewProj, so an
// SV_Position-indexed Load is pixel-exact under DLSS/FSR2.
#if MS_IS_WATER || (defined(MULTIPLY_BLEND) && MULTIPLY_BLEND)
    #define MS_TRACED_REFL_FOLD 0
#else
    #define MS_TRACED_REFL_FOLD 1
#endif
// The FRONT-LAYER tier additionally mirrors the prepass's packet predicate
// (TranslucentFrontLayerPass::IsFrontLayerPacket): Additive draws are never
// stamped (they add light, they do not reflect it), so they must never fold a
// stamp either. Billboard cannot be tested here (BILLBOARD is a VS-only bit,
// masked out of the surface template) and Unlit returns before the BRDF; both
// are caught by the two-sided depth identity test in
// MS_FetchFrontLayerReflection instead. Keep the two sets in step.
#if MS_TRACED_REFL_FOLD && !(defined(ADDITIVE_BLEND) && ADDITIVE_BLEND)
    #define MS_FRONTLAYER_FOLD 1
#else
    #define MS_FRONTLAYER_FOLD 0
#endif

// Reverse-Z NDC -> linear view depth for this view's finite perspective projection (the
// Renderer builds XMMatrixPerspectiveFovLH(fov, aspect, farZ, nearZ) - near/far swapped, not
// an infinite far plane), so zNdc = N(F-z)/((F-N)z) and the N/z approximation is good only
// far from the far plane. Same expression as HR_LinearizeReverseZ (HybridReflCommon); the
// planes come from LightCB (gFwdReflNearZ / gFwdReflFarZ), rewritten every frame.
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float MS_ScreenTraceLinearize(float zNdc)
{
    return LinearizeReverseZ(zNdc, gFwdReflNearZ, gFwdReflFarZ);
}

// The FRONT-LAYER trace: RTReflectionPass's second instance traced FROM the
// nearest translucent surface (the TranslucentFrontLayerPass stamp), so there
// is no transferability question the way there is for the opaque reuse below
// -- the only question is whether THIS fragment is the one the prepass
// stamped. Both passes rasterise the same triangles through the same vertex
// shader under the same jittered viewProj, so the front layer's SV_Position.z
// and this fragment's agree to rounding; a deeper translucent layer (a bottle
// behind a window) differs by orders of magnitude more than the tolerance
// and keeps the screen tiers instead. Reverse-Z: sky/no layer is EXACTLY 0.
// weight = conf * strength; rgb = reflected radiance (pre-Fresnel, linear).
void MS_FetchFrontLayerReflection(MaterialPixelParams p,
                                  out float3 radiance, out float weight)
{
    radiance = float3(0, 0, 0);
    weight   = 0.0;
#if MS_FRONTLAYER_FOLD
    const float strength = saturate(abs(gFwdReflStrength));
    if (strength <= 0.0 || BindlessNone(gFwdReflFrontRtrIdx) || BindlessNone(gFwdReflFrontDepthIdx))
        return;
    const int3 px = int3(int2(p.svPosition.xy), 0);
    Texture2D<float> frontDepth = BindlessTex2D<float>(gFwdReflFrontDepthIdx);
    const float zFront = frontDepth.Load(px);
    if (zFront <= 0.0) return;                          // no translucent layer here
    // Two-sided identity: only the fragment that produced the stamp may fold it. The stamp
    // and this draw rasterise the same triangles through the same VS (GBuffer_VS's
    // relevant-perm mask is BILLBOARD only, so both passes resolve to one shader), the same
    // jittered PerViewCB, the same cull / front-face state and no depth bias, so the stamped
    // fragment's SV_Position.z equals the stored D32 value to rounding; the relative slack
    // (plus an absolute floor for far, tiny reverse-Z values) is belt and braces. A deeper
    // layer (a bottle behind a window) is orders of magnitude below; a nearer unstamped one
    // (an additive glow card, a billboard sprite, a sub-1/255-opacity texel the prepass
    // clipped) must not inherit the pane's trace, so a one-sided test is not enough.
    const float z     = p.svPosition.z;
    const float slack = max(zFront * 1e-5, 1e-9);
    if (abs(z - zFront) > slack) return;                // not the stamped fragment
    Texture2D<float4> traced = BindlessTex2D<float4>(gFwdReflFrontRtrIdx);
    const float4 t = traced.Load(px);
    weight   = saturate(t.a) * strength;
    radiance = t.rgb;
#endif
}

// `rtConf` is also returned so the debug paint (below) can tell "gated out" from "nothing
// traced here". gFwdReflStrength carries its sign as a mode bit: magnitude = strength,
// negative = DX12_FWDREFL_DEBUG paint (an environment variable the renderer reads).
void MS_FetchTracedReflection(MaterialPixelParams p, float3 N,
                              out float3 radiance, out float weight, out float rtConf)
{
    radiance = float3(0, 0, 0);
    weight   = 0.0;
    rtConf   = 0.0;
#if MS_TRACED_REFL_FOLD
    const float strength = abs(gFwdReflStrength);
    if (strength <= 0.0 || BindlessNone(gFwdReflNormalIdx) || BindlessNone(gFwdReflDepthIdx))
        return;
    const int3 px = int3(int2(p.svPosition.xy), 0);

    Texture2D<float4> rtReflectionTex = BindlessTex2D<float4>(rtReflectionIdx);
    const float4 traced = rtReflectionTex.Load(px);
    rtConf = saturate(traced.a);
    if (rtConf <= 0.0)
        return;

    // Reverse-Z NDC depth of the opaque surface behind this fragment (grass-
    // free snapshot; sky is EXACTLY 0.0 - never test with an epsilon) and of
    // this fragment itself (SV_Position.z is the post-viewport depth, the same
    // convention the depth buffer holds).
    Texture2D<float> depthTex = BindlessTex2D<float>(gFwdReflDepthIdx);
    const float zOpaque = depthTex.Load(px).r;
    const float zGlass  = p.svPosition.z;
    if (zOpaque <= 0.0 || zGlass <= 0.0)
        return;
    // zOpaque <= zGlass (reverse-Z) because the opaque surface passed the
    // depth test BEHIND us; linearised, linGlass <= linOpaque, equal when
    // touching. Perspective only - an orthographic camera has no such ratio,
    // but RTR is gated off for one anyway (Renderer's rtrActive gate), so
    // rtConf is already 0 there and this is never reached.
    const float linOpaque = MS_ScreenTraceLinearize(zOpaque);
    const float linGlass  = MS_ScreenTraceLinearize(zGlass);
    const float ratio     = saturate(linGlass / max(linOpaque, 1e-4));
    const float proximity = smoothstep(0.5, 0.8, ratio);

    Texture2D<float4> normalTex = BindlessTex2D<float4>(gFwdReflNormalIdx);
    const float3 Nopaque  = normalize(normalTex.Load(px).xyz * 2.0 - 1.0);
    const float  coplanar = smoothstep(0.94, 0.99, dot(N, Nopaque));

    weight   = rtConf * coplanar * proximity * saturate(strength);
    radiance = traced.rgb;
#endif
}

// ==== The translucent surface's own screen-space reflection ====
// The reuse above transfers only when the surface behind the glass stands in for the glass.
// Everything else - a tilted pane, glass far in front of what it covers, a window onto the
// street, a bottle - needs a reflection ray that starts on the translucent surface itself,
// as UE's front-layer translucency reflections, HDRP's SSR-Transparent and Frostbite do: one
// screen-space march per translucent pixel against the opaque depth, radiance from the
// previous frame's colour pyramid, probe/sky fallback on a miss, no denoiser.
// RTReflectionPass's screen tier (HybridReflIntersectSW) is a compute pass with a Hi-Z
// pyramid and its own root signature, not callable from here, so this is a deliberately
// simple mip-0 march over the same depth snapshot it builds its Hi-Z from (SSRTraceDepth:
// opaque + terrain, no grass, water or transparents - a pane never self-intersects and a ray
// passes through the other panes, the accepted single-layer limit of every engine above).
//
// Geometry: the segment [origin, origin + R*rayMax] is interpolated in homogeneous clip
// space. viewProj is affine on homogeneous coordinates, so the clip-space lerp parameter is
// the world-space one: the hit is `origin + R * (s * worldLen)` with no inverse projection,
// and the ray's NDC depth at every step is perspective-correct for free (what
// HybridReflIntersectSW's HR_EvalRayNdcZ reconstructs by hand).
//
// Confidence = thickness residual x front-facing hit x screen-edge vignette, every term a
// smoothstep (a hard cut stripes at grazing angles). Radiance = the pyramid at the hit
// reprojected with prevViewProj, the cone mip chosen as HR_FetchPrevRadiance does,
// firefly-capped at 64 like the NRD write sites. Cost: up to 64 coarse depth loads plus 6
// refinement loads per crossing (usually one) and 2 at the hit, per translucent pixel below
// the roughness band; foliage cards and rough VFX skip it. On an interior frame the total is
// within run-to-run noise of the trace being off.
//
// Hard limits, by construction: nothing runs without a pyramid (no post-graph SSR chain last
// frame / orthographic camera: gFwdReflPrevHdrIdx is 0); the pyramid has a black sky, so a
// ray that exits the screen or lands on sky depth is a miss that falls back to the probe/sky
// tier; other translucent surfaces and particles are absent from the pyramid; one frame of
// latency on moving content; and no disocclusion validation - HR_FetchPrevRadiance
// cross-checks a prev-frame depth and the velocity buffer, this fetch has neither bound, so
// a hit hidden last frame reads whatever was at that pixel (a one-frame ghost on fast
// disocclusion). The traced direction is the isotropic reflect(-V, N) like every traced
// tier; only the probe/sky fallback uses the anisotropy-bent normal.
// TODO(Reflections_Refactor_Plan 7): Hi-Z acceleration, velocity / prev-depth disocclusion
// validation, and a per-material opt-out.

bool MS_ScreenTraceReflection(float3 worldPos, float3 N, float3 V, float roughness,
                              out float3 radiance, out float conf)
{
    radiance = float3(0, 0, 0);
    conf     = 0.0;
#if MS_TRACED_REFL_FOLD
    if (abs(gFwdReflStrength) <= 0.0 || BindlessNone(gFwdReflPrevHdrIdx) ||
        BindlessNone(gFwdReflDepthIdx) || BindlessNone(gFwdReflNormalIdx))
        return false;
    // Same band as the opaque tiers: above it the lobe is too wide for a
    // single ray to say anything, and the probe/sky tier is the right answer.
    if (roughness > gFwdReflRoughMax)
        return false;
    // A shading normal facing away from the viewer (normal-mapped silhouette,
    // a back face that slipped through) would send R roughly along the view
    // ray and the origin bias AWAY from the camera: the march would then
    // "reflect" the surface directly behind the pane. Probes/sky instead.
    if (dot(N, V) <= 0.0)
        return false;

    Texture2D<float>  depthTex  = BindlessTex2D<float>(gFwdReflDepthIdx);
    Texture2D<float4> normalTex = BindlessTex2D<float4>(gFwdReflNormalIdx);
    Texture2D<float4> prevTex   = BindlessTex2D<float4>(gFwdReflPrevHdrIdx);
    uint dw, dh;
    depthTex.GetDimensions(dw, dh);
    const float2 dim = float2(dw, dh);

    const float3 R      = reflect(-V, N);
    const float3 origin = worldPos + N * 0.02;      // off the surface, along N
    const float4 c0 = mul(float4(origin, 1.0), viewProj);
    if (c0.w <= 1e-3)
        return false;
    float  worldLen = gFwdReflRayMax;
    float4 c1 = mul(float4(origin + R * worldLen, 1.0), viewProj);
    if (c1.w <= 1e-3)
    {
        // The far end crossed the camera plane: shrink the segment until its
        // projection is numerically safe (HybridReflIntersectSW does the same).
        const float sShrink = clamp((1e-3 - c0.w) / (c1.w - c0.w) * 0.99, 1e-4, 1.0);
        c1       = lerp(c0, c1, sShrink);
        worldLen *= sShrink;
    }

    // Coarse march, then a binary refinement of every crossing before the thickness verdict.
    // Do not judge thickness on the coarse sample (the classic linear-SSR mistake): with a
    // 150 wu ray in 64 steps a sample lands up to ~2.3 wu behind the surface it just
    // crossed, which a 0.2 wu thickness reads as "thin foreground object, keep going", and
    // the ray then walks behind everything and never hits. Refining first lands within a few
    // cm of the true crossing, so the thickness test rejects only a genuinely thin occluder
    // the ray passed behind.
    const int kSteps = 64;
    float  sPrev  = 0.0;
    float  sHit   = 0.0;
    float  resid  = 0.0;      // ray-behind-surface distance at the hit, linear
    float  linScene = 0.0;
    float  tolHit = 0.2;      // the thickness the hit was accepted under
    float2 uvHit  = float2(0, 0);
    bool   hit    = false;
    [loop]
    for (int i = 1; i <= kSteps && !hit; ++i)
    {
        const float  s   = float(i) / float(kSteps);
        const float4 c   = lerp(c0, c1, s);
        const float3 ndc = c.xyz / c.w;
        const float2 uv  = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (any(uv < 0.0) || any(uv > 1.0))
            break;                                   // left the screen: miss
        const float zScene = depthTex.Load(int3(int2(uv * dim), 0)).r;
        // Reverse-Z: a surface NEARER than the ray point (larger zNdc) means
        // the ray has gone behind it. Sky is EXACTLY 0.0 and never nearer.
        if (zScene > 0.0 && zScene >= ndc.z)
        {
            // Refine the crossing between the last in-front sample and this one.
            float sA = sPrev, sB = s;
            [unroll]
            for (int k = 0; k < 6; ++k)
            {
                const float  sm   = 0.5 * (sA + sB);
                const float4 cm   = lerp(c0, c1, sm);
                const float3 ndcm = cm.xyz / cm.w;
                const float2 uvm  = saturate(float2(ndcm.x * 0.5 + 0.5, 0.5 - ndcm.y * 0.5));
                const float  zm   = depthTex.Load(int3(int2(uvm * dim), 0)).r;
                if (zm > 0.0 && zm >= ndcm.z) sB = sm; else sA = sm;
            }
            const float4 cb   = lerp(c0, c1, sB);
            const float3 ndcb = cb.xyz / cb.w;
            const float2 uvb  = saturate(float2(ndcb.x * 0.5 + 0.5, 0.5 - ndcb.y * 0.5));
            const float  zb   = depthTex.Load(int3(int2(uvb * dim), 0)).r;
            if (zb > 0.0)
            {
                const float linRay = MS_ScreenTraceLinearize(ndcb.z);
                const float linS   = MS_ScreenTraceLinearize(zb);
                // Thickness the surface is assumed to have: the SW tier's floor
                // (0.2 wu, AMD's default) grown with distance, plus a little
                // for the ray length (a long ray's refinement is coarser).
                const float thick = max(0.2, linS * 0.004) + sB * worldLen * 0.002;
                const float r     = linRay - linS;
                if (r < thick)
                {
                    hit = true; sHit = sB; resid = max(r, 0.0);
                    linScene = linS; uvHit = uvb; tolHit = thick;
                }
            }
            // Not a hit: the ray passed behind a thin object (a lamp post, a railing) - keep
            // marching, the surface we want is further along.
        }
        sPrev = s;
    }
    if (!hit)
        return false;

    const int2  pxHit     = int2(clamp(uvHit * dim, float2(0, 0), dim - 1.0));
    // Graded against the SAME thickness the hit was accepted under, so a
    // crossing the march stops on can never be thrown away here for a
    // tolerance it was never tested against.
    const float residConf = 1.0 - smoothstep(0.0, tolHit, resid);

    // Back-facing / grazing landings fade out (a ray cannot see a surface whose normal
    // points away from where it came from). The normal is the live GBuffer1 while the hit
    // came from the grass-/water-free snapshot, so over a grass or water pixel the two
    // describe different surfaces - the same pairing the opaque screen tier
    // (HybridReflIntersectSW) has, accepted rather than special-cased.
    const float3 hitN      = normalize(normalTex.Load(int3(pxHit, 0)).xyz * 2.0 - 1.0);
    const float  frontConf = smoothstep(0.0, 0.2, dot(hitN, -R));
    // Screen-edge vignette: content at the border is about to leave the
    // pyramid, and the reprojection below gets less reliable there.
    const float2 edge     = min(uvHit, 1.0 - uvHit);
    const float  edgeConf = smoothstep(0.0, 0.08, min(edge.x, edge.y));

    // `conf` stays 0 until the RADIANCE is in hand: the caller folds
    // `radiance` by `conf` without looking at the bool, so a confidence
    // committed before one of the reprojection rejections below would fold
    // BLACK at full weight and turn the surface's probe/sky reflection off.
    const float geomConf = residConf * frontConf * edgeConf;
    if (geomConf <= 0.0)
        return false;

    // Radiance: the hit as seen LAST frame (the pyramid is last frame's
    // reflection-free opaque scene). Static-scene reprojection through
    // PerViewCB's prevViewProj, which is the UNJITTERED previous matrix while
    // the pyramid holds jittered content: a sub-pixel (<= 0.5 px) bias that
    // the cone-mip filtering absorbs. A mv-based fetch would need the velocity
    // buffer bound here as well; AMD's SSSR samples history the same way.
    const float  rayLen = sHit * worldLen;
    const float3 hitWS  = origin + R * rayLen;
    const float4 pc     = mul(float4(hitWS, 1.0), prevViewProj);
    if (pc.w <= 1e-3)
        return false;
    // + last frame's jitter: the pyramid was rasterised on the jittered grid.
    float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5)
               + gFwdReflPrevJitterUV;
    if (any(puv <= 0.0) || any(puv >= 1.0))
        return false;

    // Cone-footprint mip, the HR_FetchPrevRadiance shape: lobe radius grows
    // with roughness^2 x ray length, converted to pixels at the hit depth;
    // never sharper than the roughness-only pick, never past the top mip.
    const float roughFactor = saturate(roughness / max(gFwdReflRoughMax, 1e-4));
    const float coneRadius  = roughness * roughness * rayLen;
    const float pixelRadius = coneRadius * dim.y * 0.5 / max(linScene, 0.1);
    const float coneMip     = clamp(log2(max(pixelRadius, 1.0)), 0.0, 4.0);
    const float mip         = min(max(roughFactor * 3.0, coneMip), gFwdReflPrevHdrMaxMip);
    // gIBLSampler WRAPS (it is the cubemap sampler): keep the bilinear
    // footprint of the mip we actually sample inside the texture, or a border
    // fetch pulls the opposite screen edge in. One texel at mip m is 2^m
    // mip-0 pixels wide.
    const float2 guard = exp2(mip) * 0.5 / dim;
    puv = clamp(puv, guard, 1.0 - guard);
    float3 rad = prevTex.SampleLevel(gIBLSampler, puv, mip).rgb;
    if (any(isnan(rad)) || any(isinf(rad)))
        return false;
    radiance = min(max(rad, 0.0), 64.0);          // NRD write-site firefly cap
    conf     = geomConf;
    return true;
#else
    return false;
#endif
}

// ==== Forward parity: the screen-space terms ====
// The RT sun-shadow mask (RTShadowPass) and the screen AO (XeGTAO / RTAO - whatever
// LightingPass samples at t18) are computed for the opaque surface at this pixel. On a
// translucent fragment they answer for what is behind the glass, so they are folded only as
// far as that surface stands in for the glass: the same linearised-depth proximity the
// coplanar reflection reuse uses (MS_FetchTracedReflection). A pane on a wall gets the
// wall's mask and AO; a wine glass far in front of a wall keeps the CSM / DDGI answer; glass
// against the sky (opaque depth 0) gets nothing - mask and AO are 1 there anyway. The opaque
// depth is the SSRTraceDepth snapshot (LightCB.fwdReflDepthIdx, named every frame
// TransparentPass exists, independent of the reflection toggle); the planes come from
// fwdReflNearZ / FarZ.
float MS_ForwardOpaqueProximity(MaterialPixelParams p)
{
    if (BindlessNone(gFwdReflDepthIdx)) return 0.0;
    Texture2D<float> opaqueDepth = BindlessTex2D<float>(gFwdReflDepthIdx);
    const float zOpaque = opaqueDepth.Load(int3(int2(p.svPosition.xy), 0));
    if (zOpaque <= 0.0) return 0.0;                              // sky behind us
    const float linGlass  = MS_ScreenTraceLinearize(p.svPosition.z);
    const float linOpaque = MS_ScreenTraceLinearize(zOpaque);
    // An absolute gap band, in metres: "a pane on a wall" is a physical statement (5 cm ->
    // 60 cm), not a ratio. The reflection reuse can use a ratio because a coplanar cosine
    // runs alongside it; here the gate is the whole decision, and a ratio is scale-relative
    // (0.8 lets a bottle 7 m in front of a 30 m wall inherit the wall's sun shadow - a
    // plausible-looking darkening nobody would spot). The ratio survives only as a far-field
    // floor beyond 30 m, where a 60 cm band is below D32 reverse-Z precision.
    const float gap  = max(linOpaque - linGlass, 0.0);
    const float wAbs = 1.0 - smoothstep(0.05, 0.60, gap);
    const float wRel = smoothstep(0.5, 0.8, linGlass / max(linOpaque, 1e-4))
                     * step(30.0, linGlass);
    return max(wAbs, wRel);
}

// AO only: a wall's ambient occlusion transfers to a pane lying on it, not to a curved glass
// or a bottle standing in front of it, so the AO fold also takes the coplanar term the
// reflection reuse has (GBuffer1 normal, named every frame in LightCB.fwdReflNormalIdx). The
// sun mask must not take it: a caster's shadow falls on the pane and the wall alike whatever
// their angle.
float MS_ForwardOpaqueCoplanar(MaterialPixelParams p, float3 N)
{
    if (BindlessNone(gFwdReflNormalIdx)) return 0.0;
    Texture2D<float4> gbufNormal = BindlessTex2D<float4>(gFwdReflNormalIdx);
    const float3 Nop = normalize(gbufNormal.Load(int3(int2(p.svPosition.xy), 0)).xyz * 2.0 - 1.0);
    return smoothstep(0.80, 0.97, dot(N, Nop));
}

// RT sun-shadow mask fold, proximity-gated; 1 = lit. Same gate as shadow.hlsli's
// RT_SHADOW_MASK block (LightCB.rtShadowEnabled): the fallback texture is 1x1 and must never
// be Load()ed full-screen.
float MS_ForwardRTShadowMask(MaterialPixelParams p, float proximity)
{
    if (rtShadowEnabled == 0.0 || proximity <= 0.0) return 1.0;
    // Its largest channel: the forward surfaces take the grey of glass shadows, not their colour.
    Texture2D<float4> maskTex = BindlessTex2D<float4>(rtShadowMaskIdx);
    const float m = maskTex.Load(int3(int2(p.svPosition.xy), 0)).a;
    return lerp(1.0, m, proximity);
}

// Screen AO fold, proximity-gated. LightCB.fwdAoIdx names the SAME texture
// LightingPass samples (RTAO output or XeGTAO aoFinal, render resolution,
// Texture2D<float>); 0 = no AO source this frame.
float MS_ForwardScreenAO(MaterialPixelParams p, float proximity)
{
    if (BindlessNone(gFwdAoIdx) || proximity <= 0.0) return 1.0;
    Texture2D<float> aoTex = BindlessTex2D<float>(gFwdAoIdx);
    const float a = aoTex.Load(int3(int2(p.svPosition.xy), 0)).r;
    return lerp(1.0, a, proximity);
}

// ==== The deferred pass's surface evaluation ====
// SurfaceShading / EvalSurfaceBRDF / SurfaceDiffuseNoL / EvalSurfaceTransmission and the
// per-light PunctualLightReach / EvalPunctualSurface: one copy, shared with Lighting.ps.
#include "surface_eval.hlsli"
#include "toon_eval.hlsli"   // the toon model's per-light maths (forward toon, below)

// The forward path's SurfaceShading. Read straight from SurfaceData and the
// material record - no GBuffer round trip, so no encode/decode and no face
// rules - with the deferred pass's clamps (Lighting.ps fills the same struct
// from RT4), so a translucent anisotropic, subsurface, foliage or coated
// material shades like an opaque one.
SurfaceShading MS_ForwardSurfaceShading(MaterialGPUData mat, SurfaceData s, float3 N)
{
    SurfaceShading sm = InitSurfaceShading();
    sm.model = mat.customShadingModel;
    if (sm.model == SHADING_MODEL_ANISOTROPIC)
    {
        // Gram-Schmidt against the SHADING normal: the hook may have written a
        // normal-mapped normalWS that the seeded (geometric) tangent is no
        // longer perpendicular to.
        const float3 t  = s.anisotropyDirWS - N * dot(N, s.anisotropyDirWS);
        const float  l2 = dot(t, t);
        sm.tangentWS   = (l2 > 1e-8) ? t * rsqrt(l2) : SM_DecodeAnisoDir(N, 0.0);
        sm.bitangentWS = cross(N, sm.tangentWS);
        sm.anisotropy  = s.anisotropy;
    }
    else if (sm.model == SHADING_MODEL_SUBSURFACE)
    {
        sm.sssTint    = max(mat.subsurfaceColor, 0.0)
                      * max(mat.subsurfaceStrength, 0.0)
                      * saturate(s.subsurfaceMask);
        sm.sssOpacity = saturate(s.subsurfaceOpacity);
        sm.sssWrap    = saturate(mat.subsurfaceWrap);
    }
    else if (sm.model == SHADING_MODEL_CLEARCOAT)
    {
        sm.clearCoat          = saturate(s.clearCoat);
        sm.clearCoatRoughness = saturate(s.clearCoatRoughness);
    }
    else if (sm.model == SHADING_MODEL_TWOSIDED_FOLIAGE)
    {
        // The named subsurface fields - for a custom shader too: the material packer copies
        // its foliage parameters into them (the deferred resolve's rule, see
        // ShadingModel.h).
        sm.foliageWrap = saturate(mat.subsurfaceWrap);
        sm.foliageTint = max(mat.subsurfaceColor, 0.0)
                       * max(mat.subsurfaceStrength, 0.0);
    }
    return sm;
}

// ==== Forward toon ====
// A Toon material in a forward (blend) pass takes the deferred toon's direct light for a
// non-face part, read from the material record: each light through the ramp in place of N.L
// (the sharpened half-lambert form term, toon_eval.hlsli), the GGX highlight on the coupled
// light, the sun's spec-ramp glint, the direct-occlusion and light-scale knobs -
// Lighting.ps's toon block and AccumClusteredLight's NPR tail, line for line. Without a ramp
// texture the material's two ramp colours stand in for it, as in the deferred pass, and
// there is no glint. The ambient is the toon pass's too (MS_ForwardLighting's IBL block):
// isotropic sky / DDGI / sky visibility, no SSGI or ReSTIR GI transfer, a Fresnel-free
// diffuse share, and the ambient / specular light scales. Not ported: the face SDF and the
// direct-additive mask (forward has no RT4), the screen-space depth rim (a translucent
// surface is not in the depth buffer), and the path-traced ambient (forward has no
// path-tracer tier).
struct MSForwardToon
{
    int   rampIdx;            // -1: no ramp texture, the two colours below
    float3 rampColShadow, rampColLit;
    bool  rampHasSpecRow;     // a 1-row ramp has no spec row (Lighting.ps's note)
    float threshold, sharp, shadowStrength;
    float directScale, additionalScale, specularScale, ambientScale;
    float colorWeight, intensityWeight, referenceLuminance, mainLightLerp;
    float directOcc;
    bool  shadowEdge;         // MAT_FLAG_NPR_TOON_SHADOW_EDGE
    float sceneShadowSmooth;
};

MSForwardToon MS_ForwardToon(MaterialGPUData m, int rampIdx, float ao)
{
    MSForwardToon t;
    t.rampIdx       = rampIdx;
    t.rampColShadow = m.nprShadowRampColor;
    t.rampColLit    = m.nprDiffuseRampColor;
    t.rampHasSpecRow = false;
    if (rampIdx >= 0)
    {
        uint rw, rh;
        g_AllTextures[NonUniformResourceIndex(rampIdx)].GetDimensions(rw, rh);
        t.rampHasSpecRow = (rh >= 2u);
    }
    t.threshold          = m.nprShadowThreshold;
    t.sharp              = ToonNdlSharp(m.nprShadowSmooth);
    t.shadowStrength     = saturate(m.nprShadowStrength);
    t.directScale        = max(m.nprDirectLightScale, 0.0);
    t.additionalScale    = max(m.nprAdditionalLightScale, 0.0);
    t.specularScale      = max(m.nprSpecularLightScale, 0.0);
    t.ambientScale       = max(m.nprAmbientLightScale, 0.0);
    t.colorWeight        = m.nprSceneLightColorWeight;
    t.intensityWeight    = m.nprSceneLightIntensityWeight;
    t.referenceLuminance = m.nprReferenceLightLuminance;
    t.mainLightLerp      = saturate(m.nprMainLightLerp);
    t.directOcc          = lerp(1.0 - saturate(m.nprDirectOcclusion), 1.0, ao);
    t.shadowEdge         = (m.materialFlags & MAT_FLAG_NPR_TOON_SHADOW_EDGE) != 0u;
    t.sceneShadowSmooth  = saturate(m.nprSceneShadowSmooth);
    return t;
}

// The ramp at (term, row). s0 here is g_LinearWrap, and a wrapping sampler
// would blend the lit end with the shadow end at U = 1: clamping U to the end
// texels' centres reads exactly what the deferred pass's clamp sampler reads.
// No ramp texture: the two colours, for every row the caller asks for (the
// spec row is only asked for when rampHasSpecRow), as Lighting.ps does.
float3 MS_ForwardToonRamp(MSForwardToon t, float term, float rowV)
{
    if (t.rampIdx < 0)
        return lerp(t.rampColShadow, t.rampColLit, saturate(term));
    Texture2D ramp = g_AllTextures[NonUniformResourceIndex(t.rampIdx)];
    uint w, h;
    ramp.GetDimensions(w, h);
    const float halfTexel = 0.5 / max(float(w), 1.0);
    return ramp.SampleLevel(g_LinearWrap, float2(clamp(term, halfTexel, 1.0 - halfTexel), rowV), 0).rgb;
}

float3 MS_ForwardToonCouple(MSForwardToon t, float3 sceneLight)
{
    return ToonCoupleSceneLight(sceneLight, t.colorWeight, t.intensityWeight, t.referenceLuminance);
}

// ==== Shared forward lighting ====
// The engine's forward BRDF: direct sun (GGX) + IBL/probes + flat ambient, driven entirely
// by the SurfaceData the hook filled. The translucent main and the water main both call it,
// so a water material is lit by exactly the same model as any other surface; there is no
// separate "water BRDF" to keep in sync.
//
// Splits its result the way the glass/premultiplied packing needs:
//   bodyColor = light that passes through the surface (diffuse + diffuse IBL)
//   surfColor = light reflected off it (specular + specular IBL + emissive)
// Alpha fades body but not surf, which is why a transparent surface keeps its highlight as
// it fades out. `sunVisibility` is the physical sun visibility for this pixel, 1 = lit. It
// multiplies the two direct terms and nothing else - never the IBL terms: the
// AccumulateIndirect guarantee, written out by hand because this function is shared with
// permutations that cannot include lighting_types.hlsli. See VIS-LINEAR in Lighting.ps.hlsl.
void MS_ForwardLighting(MaterialPixelParams p, SurfaceData s, float3 worldPos,
                        float sunVisibility,
                        out float3 bodyColor, out float3 surfColor)
{
    const float3 baseColorRGB = s.baseColor;
    // Screen-space terms of the opaque surface behind this fragment, folded by
    // proximity (see MS_ForwardOpaqueProximity). Lighting.ps: ao = bakedAO * ssao.
    const float  opaqueProx = MS_ForwardOpaqueProximity(p);
#if MS_IS_WATER || (defined(MULTIPLY_BLEND) && MULTIPLY_BLEND)
    // Water: the "opaque behind" is the lakebed (the depth snapshot holds no water), whose
    // AO is not the surface's. Multiply: rgb is a factor on the background, which already
    // carries its own AO - folding it again darkens the same occlusion twice. Both keep the
    // sun-mask and cloud folds: those are how much sun reaches this point, the same question
    // for the lakebed.
    const float  aoProx = 0.0;
#else
    const float  aoProx = opaqueProx * MS_ForwardOpaqueCoplanar(p, normalize(s.normalWS));
#endif
    const float  ao        = s.ao * MS_ForwardScreenAO(p, aoProx);
    // Lighting.ps parity for the sun term: CSM (the caller) min-folded with the
    // RT sun mask (proximity-gated), then the volumetric cloud shadow. Still a
    // plain multiplier on the two DIRECT terms only (@vis-linear below). The
    // per-material receive-shadow opt-out (Lighting.ps honours it right after
    // ComputeShadowFactor) applies to the whole chain here too.
    if (g_Materials[p.materialIndex].materialFlags & MAT_FLAG_DISABLE_RECEIVE_SHADOW)
        sunVisibility = 1.0;
    else
        sunVisibility = min(sunVisibility, MS_ForwardRTShadowMask(p, opaqueProx))
                      * SampleCloudShadow(worldPos, gIBLSampler);
    const float  dbgSunVis = saturate(sunVisibility);   // debug carrier, unread
    const float  roughness = max(s.roughness, 0.045);   // BRDF stability floor
    const float  metalness = s.metallic;
    const float3 N         = normalize(s.normalWS);

    const float3 V     = normalize(cameraPos - worldPos);
    const float3 L     = normalize(-lightDir);
    const float  NdotL = saturate(dot(N, L));
    const float  NdotV = saturate(dot(N, V));

    const float  f0Scalar = 0.16 * s.reflectance * s.reflectance;
    const float3 F0       = lerp(float3(f0Scalar, f0Scalar, f0Scalar), baseColorRGB, metalness);

    // ---- Shading model, resolved once ----
    // surface_eval.hlsli's SurfaceShading, filled from SurfaceData (above); from here on the
    // sun and every punctual light run the deferred pass's own functions. isAniso / anisoT /
    // anisoB / isCoat stay for the IBL block below.
    const MaterialGPUData msMat   = g_Materials[p.materialIndex];
    const SurfaceShading  sm      = MS_ForwardSurfaceShading(msMat, s, N);
    const bool            isAniso = SurfaceIsAnisotropic(sm);
    const bool            isCoat  = (sm.model == SHADING_MODEL_CLEARCOAT)
                                  && (s.clearCoat > 0.0);
    const float3          anisoT  = sm.tangentWS;
    const float3          anisoB  = sm.bitangentWS;

    // The sun's reflected lobe - anisotropic GGX or isotropic, plus the clear
    // coat on top - exactly the deferred sun's call.
    float3 specular, kD;
    EvalSurfaceBRDF(sm, N, V, L, F0, roughness, metalness, baseColorRGB, specular, kD);

    // A Toon material takes the deferred toon's direct light (MS_ForwardToon
    // above: what is and is not ported), through its ramp texture or, without
    // one, its two ramp colours; every other model keeps the PBR lobes.
    const bool    isToon      = (sm.model == SHADING_MODEL_TOON);
    const int     toonRampIdx = isToon ? msMat.toonRampTex : -1;
    MSForwardToon toon        = (MSForwardToon)0;
    if (isToon)
        toon = MS_ForwardToon(msMat, toonRampIdx, ao);

    float3 directDiffuse, directSpecular;
    [branch]
    if (isToon)
    {
        // Lighting.ps's toon composite, sun half: the form shadow indexes the ramp's shadow
        // row in place of N.L; the GGX highlight rides the coupled light, plus the spec-ramp
        // glint (DanbaidongRP's additive form).
        const float  formShadowArea = lerp(1.0, ToonHalfLambertTerm(N, L, toon.threshold, toon.sharp),
                                           toon.shadowStrength);
        // @vis-linear — a plain multiplier, or ToonVisibility, which is
        // endpoint-pinned (exactly 0 at 0 and 1 at 1).
        const float  toonVis = toon.shadowEdge
                             ? ToonVisibility(sunVisibility, 0.5, toon.sceneShadowSmooth)
                             : saturate(sunVisibility);
        const float3 kd        = baseColorRGB * (1.0 - metalness);
        const float3 coupled   = MS_ForwardToonCouple(toon, lightColor);
        const float3 toonLight = lerp(coupled, float3(1.0, 1.0, 1.0), toon.mainLightLerp);
        const float3 shadowRampCol = MS_ForwardToonRamp(toon, formShadowArea,
                                                        TOON_RAMP_V_DIRECT_SHADOW);
        directDiffuse = kd * shadowRampCol * toonLight * (toon.directScale / PI)
                      * toon.directOcc * toonVis;
        float3 toonSpec = specular * coupled * NdotL * toon.directScale * toonVis;
        if (toon.rampHasSpecRow)
        {
            // U = the sun BRDF's peak proximity, not the shadowed luminance.
            const float3 f0          = lerp(float3(0.04, 0.04, 0.04), baseColorRGB, metalness);
            const float3 specRampCol = MS_ForwardToonRamp(toon, saturate(Luminance(specular)),
                                                          TOON_RAMP_V_DIRECT_SPEC);
            toonSpec += f0 * specRampCol * NdotL * toon.directScale
                      * toonLight * toonVis;
        }
        directSpecular = toonSpec * toon.directOcc * toon.specularScale;
    }
    else
    {
        const float3 diffuse  = kD * baseColorRGB / PI;
        // Subsurface softens the diffuse terminator only - specular keeps the true N.L, or
        // the highlight smears onto the shadow side.
        const float  diffNoL  = SurfaceDiffuseNoL(sm, dot(N, L));
        // @vis-linear — in this branch sunVisibility appears exactly three times,
        // all three as a plain multiplier on a DIRECT term. It must never reach
        // the IBL block below.
        const float vis = saturate(sunVisibility);
        directDiffuse  = diffuse  * lightColor * diffNoL * vis;
        directSpecular = specular * lightColor * NdotL   * vis;

        // Light that entered the far side and comes back out toward the viewer -
        // two-sided foliage and subsurface (EvalSurfaceTransmission). Body, not
        // surface: it is transmitted, so alpha fades it like diffuse.
        if (SurfaceHasTransmission(sm))
            directDiffuse += EvalSurfaceTransmission(sm, N, V, L, ao) * lightColor * vis;
    }

#if !MS_IS_WATER
    // ---- Clustered point / spot lights ----
    // Lighting.ps's AccumClusteredLight through the same surface_eval.hlsli calls: the same
    // falloff and early-outs (PunctualLightReach), the same spot / point shadows
    // (punctual_shadow.hlsli) and the same lobes, including each light's foliage / subsurface
    // transmission (EvalPunctualSurface). Each light's own shadow is a
    // plain multiplier on its DIRECT terms (@vis-linear).
    if (clusterLightCount > 0u)
    {
        // Geometric receiver derivatives, BEFORE the divergent loop
        // (SampleSpotShadow's depth-gradient fit; Lighting.ps does the same).
        const float3 worldDx = ddx(worldPos);
        const float3 worldDy = ddy(worldPos);
        const bool   receiveShadow = (msMat.materialFlags & MAT_FLAG_DISABLE_RECEIVE_SHADOW) == 0u;
        // gWaterRTSize = the RENDER resolution, written every frame for every
        // forward pass (main and preview LightCBs) - the cluster grid's space.
        const float2 rtSize = max(gWaterRTSize.xy, float2(1.0, 1.0));
        const float  viewZ  = abs(mul(float4(worldPos, 1.0), viewMatrix).z);
        const uint   key    = ClusterIndex(GetClusterCoord(p.svPosition.xy / rtSize, viewZ,
                                                           clusterNearZ, clusterFarZ));
        const uint   offset = gLightGrid[key].offset;
        const uint   count  = gLightGrid[key].count;
        float3 toonAddColor = float3(0, 0, 0), toonPunctualSpec = float3(0, 0, 0);
        for (uint li = 0; li < count; ++li)
        {
            uint lightIdx = li;
            if (offset != 0xFFFFFFFFu) lightIdx = gLightIndexList[offset + li];
            const GPULight light = gLights[lightIdx];
            // The sun (type 0) is the block above. Out of range, outside the
            // cone, or behind a surface that neither transmits nor wraps: nothing.
            PunctualReach r;
            if (!PunctualLightReach(light, worldPos, N, sm, r)) continue;
            float lightVis = 1.0;
            if (receiveShadow && light.type == 2u && light.shadowSliceIdx != 0xFFFFFFFFu)
                lightVis = SampleSpotShadow(worldPos, worldDx, worldDy, N, light.position,
                                            light.spotAngle, light.shadowSliceIdx);
            else if (receiveShadow && light.type == 1u && light.shadowSliceIdx != 0xFFFFFFFFu)
                lightVis = SamplePointShadow(worldPos, N, light.position, light.radius,
                                             light.shadowSliceIdx);
            if (!(lightVis > 0.0)) continue;
            const float3 radiance = light.color * light.intensity * (r.atten * r.spotFactor);
            DirectLighting d;
            float3 specL, kDl;
            EvalPunctualSurface(r, radiance, N, V, F0, baseColorRGB, roughness, metalness, ao,
                                sm, d, specL, kDl);
            [branch]
            if (isToon)
            {
                // AccumClusteredLight's NPR tail (non-face): THIS light's
                // own form term through the ramp's additional-light row, its GGX on
                // the coupled radiance, both under the additional-light scale.
                const float3 nprRadiance = MS_ForwardToonCouple(toon, light.color * light.intensity)
                                         * (r.atten * r.spotFactor) * lightVis;
                const float  addTerm     = ToonHalfLambertTerm(N, r.L, toon.threshold, toon.sharp);
                toonPunctualSpec += specL * nprRadiance * r.NdL * toon.additionalScale;
                toonAddColor     += kDl * nprRadiance
                                  * MS_ForwardToonRamp(toon, addTerm, TOON_RAMP_V_ADD_SHADOW)
                                  * (toon.additionalScale / PI);
            }
            else
            {
                // Transmitted light is body, like the sun's (alpha fades it).
                directDiffuse  += (d.diffuse + d.transmission) * lightVis;
                directSpecular += d.specular * lightVis;
            }
        }
        // The toon composite's punctual half (Lighting.ps): the per-light ramped
        // diffuse times the albedo term, both halves under the direct-occlusion knob.
        if (isToon)
        {
            directDiffuse  += baseColorRGB * (1.0 - metalness) * toonAddColor * toon.directOcc;
            directSpecular += toonPunctualSpec * toon.directOcc * toon.specularScale;
        }
    }
#endif

    float3 iblDiffuseTerm  = float3(0, 0, 0);
    // Debug-paint carriers: mode 5 reads dbgDdgiDiff / dbgSkyDiff; the coverage and
    // visibility ones are unread (DXC strips them).
    float3 dbgDdgiDiff = float3(0, 0, 0), dbgSkyDiff = float3(0, 0, 0);
    float  dbgDdgiCov = 0.0, dbgSkyVis = 1.0, dbgSsgiCov = 0.0;
    float3 iblSpecularTerm = float3(0, 0, 0);

    // Same-frame traced reflection of the opaque surface behind this pixel, weighted by how
    // transferable it is (see MS_FetchTracedReflection). Fetched before the gate so it joins
    // it, as Lighting.ps gates on `tracedReflection.a > 0.0`: the traced tier is its own
    // light-transport path and must not depend on a sky or a probe existing. The sky terms
    // inside the block are multiplied by `envW` (the two-term gate as a 0/1 weight): in
    // WorldEnvironment None / FlatColor Renderer_IBL.cpp never binds a SkyboxComponent's cubes
    // (None: the black cube, FlatColor: its uniform sky cube), and envW is a second guard.
    float3 tracedRadiance = float3(0, 0, 0);
    float  tracedW        = 0.0;
    float  tracedConf     = 0.0;
    MS_FetchTracedReflection(p, N, tracedRadiance, tracedW, tracedConf);
    const float reuseW = tracedW;                  // kept apart for the debug paint

    // The surface's OWN screen-space reflection (MS_ScreenTraceReflection).
    // It wins by its confidence; the coplanar reuse fills what it missed.
    // Both are radiance in the same space (pre-Fresnel, pre-exposure linear).
    float3 marchRad  = float3(0, 0, 0);
    float  marchConf = 0.0;
    MS_ScreenTraceReflection(worldPos, N, V, roughness, marchRad, marchConf);
    marchConf *= saturate(abs(gFwdReflStrength));
    {
        const float wReuse = tracedW * (1.0 - marchConf);
        const float wSum   = marchConf + wReuse;
        tracedRadiance = (wSum > 1e-5)
            ? (marchRad * marchConf + tracedRadiance * wReuse) / wSum
            : float3(0, 0, 0);
        tracedW = wSum;
    }

    // The surface's OWN RayQuery trace (RTReflectionPass second instance,
    // traced from the front-layer stamp). It is the real tier -- the pane's
    // own normal, off-screen hits, DDGI/probe/sky-shaded proxies -- so it wins
    // by its confidence and the two screen tiers above only fill what it did
    // not cover (deeper translucent layers, frames with no RT tier).
    float3 frontRad = float3(0, 0, 0);
    float  frontW   = 0.0;
    MS_FetchFrontLayerReflection(p, frontRad, frontW);
    {
        const float wLower = tracedW * (1.0 - frontW);
        const float wSum   = frontW + wLower;
        tracedRadiance = (wSum > 1e-5)
            ? (frontRad * frontW + tracedRadiance * wLower) / wSum
            : float3(0, 0, 0);
        tracedW = wSum;
    }

    // Gate matches Lighting.ps: probes are independent of the sky, so they must
    // keep lighting surfaces in WorldEnvironment None/FlatColor.
    const bool  envLive  = (iblRadianceMips > 0 || reflectionProbeCount > 0);
    const float envW     = envLive ? 1.0 : 0.0;
    // DDGI is its own light-transport path (Lighting.ps joins it to the same
    // gate for the same reason: a DDGI-only world with no sky and no probes
    // must still light its translucent surfaces).
    const bool  ddgiLive = (ddgiEnabled != 0 && ddgiVolumeCount > 0);
    // The flat environment ambient (None's fill; 0 in the sky modes and in FlatColor, whose
    // uniform sky cube carries it): a uniform environment's radiance, weighted inside the block
    // (Lighting.ps).
    const float3 flatAmbientL = ambientColor * ambientIntensity;
    [branch]
    if (envLive || ddgiLive || tracedW > 0.0 || !BindlessNone(gRestirGiIdx) || any(flatAmbientL > 0.0))
    {
        // A toon receiver's sky ambient is isotropic - the band-0 average, or the irradiance
        // cube's 6-axis mean - as Lighting.ps's NPR pass shades it: the ramp is the form
        // model, and an N-dependent ambient re-shades the mesh's normal planes.
        float3 skyDiffN;
        if (isToon)
        {
            if (iblUseSH != 0)
                skyDiffN = EvalSH2Isotropic();
            else
                skyDiffN = (gIrradiance.Sample(gIBLSampler, float3( 1, 0, 0)).rgb
                          + gIrradiance.Sample(gIBLSampler, float3(-1, 0, 0)).rgb
                          + gIrradiance.Sample(gIBLSampler, float3( 0, 1, 0)).rgb
                          + gIrradiance.Sample(gIBLSampler, float3( 0,-1, 0)).rgb
                          + gIrradiance.Sample(gIBLSampler, float3( 0, 0, 1)).rgb
                          + gIrradiance.Sample(gIBLSampler, float3( 0, 0,-1)).rgb)
                         * (1.0 / 6.0);
        }
        else
        {
            skyDiffN = (iblUseSH != 0) ? EvalSH2(N) : gIrradiance.Sample(gIBLSampler, N).rgb;
        }
        const float3 skyDiff = envW * skyDiffN;
        // Anisotropic environment reflection: bend the normal the reflection is
        // taken about, which stretches the reflected image along the right axis.
        // Returns N EXACTLY at anisotropy 0, so every isotropic pixel is
        // bit-unchanged. See SM_AnisotropicBentNormal.
        const float3 Nrefl = isAniso
            ? SM_AnisotropicBentNormal(N, V, anisoT, anisoB, s.anisotropy, roughness)
            : N;
        const float3 R   = reflect(-V, Nrefl);
        const float  mip = roughness * float(max(iblRadianceMips, 1u) - 1u);

        // ---- DDGI: irradiance, sky visibility, rough specular (Lighting.ps) ----
        // The same arbitration (DDGI_PickVolumes: priority, then density; the winner owns
        // its interior and hands over across its own fade) and the same combine as the
        // deferred pass, so a translucent surface inside a room sees the room's probes, not
        // the sky. Sky visibility attenuates the sky radiance cube below: a pane inside a
        // building must not mirror a sky it cannot see. Rough surfaces (foliage cards,
        // frosted glass) take the DDGI irradiance along R as their distant specular, like
        // the wall behind them. NonUniformResourceIndex: `vi` is a per-pixel loop index.
        float3 ddgiIrradiance   = float3(0, 0, 0);
        float  ddgiCoverage     = 0.0;
        float3 ddgiSpecular     = float3(0, 0, 0);
        float  ddgiSpecCoverage = 0.0;
        // DDGI's directional sky visibility (as Lighting.ps): about N for the sky's diffuse
        // fallback, along R for its reflection.
        float4 skyMoments = float4(1.0, 0.0, 0.0, 0.0);
        float  skyCov     = 0.0;
        if (ddgiLive)
        {
            StructuredBuffer<DDGIVolumeGPU> ddgiVolumes =
                BindlessSBuf<DDGIVolumeGPU>(ddgiVolumeBufIdx);
            const DDGIVolumePick pick = DDGI_PickVolumes(worldPos, ddgiVolumes);
            const bool ddgiRoughSpec = roughness > INDIRECT_SPECULAR_DDGI_ROUGH_START;
            float4 bestSample = 0, secondSample = 0;
            float4 bestSkyM = float4(1, 0, 0, 0), secondSkyM = float4(1, 0, 0, 0);
            float  bestSkyCov = 0.0, secondSkyCov = 0.0;
            float4 bestSpec = 0, secondSpec = 0;
            [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
            {
                if (!DDGI_IsPicked(pick, vi)) continue;
                const DDGIVolumeGPU vol = ddgiVolumes[vi];
                const bool isBest = (vi == (uint)pick.bestSlot);
                Texture2D<float4> volIrradiance =
                    ResourceDescriptorHeap[NonUniformResourceIndex(vol.irradianceBaseIdx + vi)];
                Texture2D<float2> volDepth =
                    ResourceDescriptorHeap[NonUniformResourceIndex(vol.depthBaseIdx + vi)];
                StructuredBuffer<DDGIProbeData> volProbeData =
                    ResourceDescriptorHeap[NonUniformResourceIndex(vol.probeDataBaseIdx + vi)];
                // The irradiance and the probes' sky-visibility moments with the same
                // per-probe visibility weights (as Lighting.ps); a toon receiver samples
                // them isotropically, as in the NPR pass.
                float4 skyM;
                const float4 sd = DDGI_SampleVolumeSkyMoments(worldPos, N, V, vol,
                                                              volIrradiance, volDepth,
                                                              volProbeData, gIBLSampler, isToon, skyM);
                const float skyMCov = sd.a;
                float4 ss = float4(0, 0, 0, 0);
                [branch] if (ddgiRoughSpec)
                    ss = DDGI_SampleVolume(worldPos, R, V, vol,
                                           volIrradiance, volDepth,
                                           volProbeData, gIBLSampler);
                if (isBest) { bestSample = sd; bestSkyM = skyM; bestSkyCov = skyMCov; bestSpec = ss; }
                else        { secondSample = sd; secondSkyM = skyM; secondSkyCov = skyMCov; secondSpec = ss; }
            }
            const float4 ddgiResolved = DDGI_CombineWinners(bestSample, secondSample, pick);
            ddgiIrradiance = ddgiResolved.rgb;
            ddgiCoverage   = ddgiResolved.a;
            skyMoments = DDGI_CombineSkyMoments(bestSkyM, bestSkyCov, secondSkyM, secondSkyCov, pick, skyCov);
            const float4 specResolved = DDGI_CombineWinners(bestSpec, secondSpec, pick);
            if (specResolved.a > 1e-4)
            {
                ddgiSpecular     = specResolved.rgb * ddgiDiffuseScale;
                ddgiSpecCoverage = specResolved.a;
            }
        }

        // ReSTIR GI / SSGI of the opaque surface behind this fragment, read once for both
        // the specular fallback and the diffuse partition below, under the same proximity x
        // coplanar gate (aoProx). A toon receiver takes none of it: the NPR pass opts toon
        // pixels out of the SSGI near field and the ReSTIR GI specular tier.
        const bool behindGiLive = !isToon && (!BindlessNone(ssgiResultIdx) || !BindlessNone(gRestirGiIdx)) && aoProx > 0.0;
        float4 behindGi = float4(0, 0, 0, 0);
        if (behindGiLive)
        {
            Texture2D<float4> gSSGI = BindlessTex2D<float4>(!BindlessNone(gRestirGiIdx) ? gRestirGiIdx : ssgiResultIdx);
            behindGi = gSSGI.Load(int3(int2(p.svPosition.xy), 0));
        }
        // ReSTIR GI's tier of the specular fallback (as Lighting.ps): its traced coverage as
        // the sky visibility and its hit radiance as the rough band, transferred by the
        // gate; DDGI keeps the rest.
        const float giSpecGate = (!BindlessNone(gRestirGiIdx) && behindGiLive && behindGi.a > 0.0) ? aoProx : 0.0;
        // The reflection's sky visibility: DDGI's ALONG R (its lobe widened by
        // roughness), the horizon guess only where no volume covers - Lighting.ps.
        const float skyDirCov     = saturate(skyCov);
        const float skyVisibility = lerp(1.0, DDGI_SkyVisibility(skyMoments, R, DDGI_SkyLobeK(roughness)), skyDirCov);
        const float skyHorizon    = lerp(IndirectSpecular_HorizonVisibility(R), 1.0, skyDirCov);
        const float specSkyVisibility = lerp(skyVisibility, 1.0 - saturate(behindGi.a), giSpecGate);
        // The sky's diffuse visibility about N (used below too) and the far field without AO
        // - the probe normalization's "here" (as Lighting.ps). (A toon receiver's is the
        // sphere average, as in the NPR pass.)
        const float  skyVisN     = isToon ? saturate(skyMoments.x)
                                          : DDGI_SkyVisibility(skyMoments, N, 2.0 / 3.0);
        const float  skyVisDiff  = lerp(1.0, skyVisN, skyDirCov);
        const float3 localFarIrr = ddgiIrradiance * ddgiDiffuseScale * ddgiCoverage
                                 + skyDiff * saturate(1.0 - ddgiCoverage)
                                 * skyVisDiff * iblStrength;

        IndirectSpecularProbeAccum probeAccum = IndirectSpecular_MakeProbeAccum();
        StructuredBuffer<ReflectionProbe> probeBuffer =
            BindlessSBuf<ReflectionProbe>(reflProbeBufIdx);
        for (uint pi = 0; pi < reflectionProbeCount && probeAccum.coverage < 0.999; ++pi)
        {
            ReflectionProbe probe = probeBuffer[pi];
            float w = ComputeProbeWeight(worldPos, probe);
            if (w <= 1e-3) continue;
            float4 sample4 = SampleProbe(probe, worldPos, R, roughness, gIBLSampler);
            const float probeNorm = (ddgiLive && (probe.flags & REFLECTION_PROBE_FLAG_NORMALIZE) != 0u)
                                  ? ProbeNorm_Scale(localFarIrr, probe.capturePosWS, N) : 1.0;
            IndirectSpecular_AddProbe(probeAccum, sample4, w, probe.intensity * probeNorm);
        }
        const float3 skySpecular = envW * gRadiance.SampleLevel(gIBLSampler, R, mip).rgb;
        // Sky tier attenuated by the DDGI sky visibility, then the rough band hands over to
        // the DDGI irradiance along R - Lighting.ps's skyTerm / roughBlend /
        // distantFallback, verbatim.
        const float3 skyTerm = skySpecular * (iblStrength * skyVisibility * skyHorizon);
        const float  roughBlend = smoothstep(INDIRECT_SPECULAR_DDGI_ROUGH_START,
                                             INDIRECT_SPECULAR_DDGI_ROUGH_END,
                                             roughness) * ddgiSpecCoverage;
        float3 distantSpecular = lerp(skyTerm, ddgiSpecular, roughBlend);
        [branch] if (giSpecGate > 0.0)
        {
            const float3 giSkyTerm = skySpecular * (iblStrength * (1.0 - saturate(behindGi.a))
                                                    * IndirectSpecular_HorizonVisibility(R));
            const float3 giDistant = giSkyTerm + smoothstep(INDIRECT_SPECULAR_DDGI_ROUGH_START,
                                                            INDIRECT_SPECULAR_DDGI_ROUGH_END, roughness)
                                               * behindGi.rgb * ddgiDiffuseScale;
            distantSpecular = lerp(distantSpecular, giDistant, giSpecGate);
        }
        // Specular occlusion is fallback-only (indirect_specular.hlsli): the traced tier
        // already carries its own visibility and must not be darkened twice.
        // Lighting.ps.hlsl applies it to fallbackRadiance before the two-tier lerp; this is
        // the same shape. envBRDF and the coat attenuation are multiplicative, so applying
        // it to the radiance equals applying it after envBRDF for the probe/sky tier.
        const float  specOcc = IndirectSpecular_SpecularOcclusion(ao, NdotV, roughness);
        const float3 fallbackRadiance = IndirectSpecular_ResolveProbeFallback(
            probeAccum, distantSpecular) * specOcc;
        // Two-tier fold, the same shape as Lighting.ps: the traced tier replaces the
        // probe/sky tier by its weight and then rides the same Fresnel / envBRDF /
        // clear-coat path below with this surface's F0 and roughness - the traced texture
        // holds pre-Fresnel radiance, as specRadiance does. At tracedW == 0 this is exactly
        // the probe/sky tier.
        const float3 specRadiance = lerp(fallbackRadiance, tracedRadiance, tracedW);

        const float3 Fibl    = FresnelSchlickRoughness(NdotV, F0, roughness);
        const float2 envBRDF = IndirectSpecular_SampleEnvBRDF(
            gBRDFLUT, gIBLSampler, NdotV, roughness);
        float3 specIBL = IndirectSpecular_ApplyEnvBRDF(specRadiance, F0, envBRDF);
        float  coatIblAtten = 1.0;
        float3 coatFlatSpec = float3(0, 0, 0);   // the film's reflection of the flat ambient
        if (isCoat)
        {
            const float  coatRough = max(s.clearCoatRoughness, 0.045);
            const float  coatMip   = coatRough * float(max(iblRadianceMips, 1u) - 1u);
            const float3 coatR     = reflect(-V, N);   // the film, not the base
            // The film's reflection is always the sky tier: occluded like the
            // base's but along ITS direction and lobe (Lighting.ps), with the
            // specular occlusion at the film's own roughness.
            const float  coatSkyVis = lerp(lerp(1.0, DDGI_SkyVisibility(skyMoments, coatR, DDGI_SkyLobeK(coatRough)),
                                                skyDirCov),
                                           1.0 - saturate(behindGi.a), giSpecGate);
            const float3 coatRad   = gRadiance.SampleLevel(gIBLSampler, coatR, coatMip).rgb
                                   * iblStrength * lerp(IndirectSpecular_HorizonVisibility(coatR), 1.0, skyDirCov)
                                   * IndirectSpecular_SpecularOcclusion(ao, NdotV, coatRough)
                                   * envW * coatSkyVis;
            const float2 coatBRDF  = IndirectSpecular_SampleEnvBRDF(
                gBRDFLUT, gIBLSampler, NdotV, coatRough);
            const float3 coatSpec  = IndirectSpecular_ApplyEnvBRDF(
                coatRad, float3(0.04, 0.04, 0.04), coatBRDF) * s.clearCoat;
            coatIblAtten = 1.0 - (0.04 + 0.96 * pow(saturate(1.0 - NdotV), 5.0)) * s.clearCoat;
            specIBL = specIBL * coatIblAtten + coatSpec;
            coatFlatSpec = IndirectSpecular_ApplyEnvBRDF(
                flatAmbientL, float3(0.04, 0.04, 0.04), coatBRDF) * s.clearCoat;
        }
        // A toon receiver's ambient diffuse takes no Fresnel share (the NPR
        // pass: that NdotV-dependent darkening reveals the mesh's normal planes).
        const float3 kDibl   = (isToon ? float3(1.0, 1.0, 1.0) : (1.0 - Fibl))
                             * (1.0 - metalness) * coatIblAtten;
        // Indirect diffuse: DDGI -> sky IBL, the Lighting.ps far-field chain verbatim (no
        // SSGI in this chain: that tier is screen-space and opaque-only). The AO split is
        // the same: DDGI already carries probe-scale visibility, so screen AO is only
        // partially re-applied to its share (ddgiAONearFieldStrength); the sky fallback
        // takes DDGI's sky visibility about N where a volume covers, and then the DDGI
        // part's AO. Reduces to `skyDiff * ao * iblStrength` exactly when no volume covers
        // the pixel.
        const float  ddgiAO       = lerp(1.0, ao, saturate(ddgiAONearFieldStrength));
        const float  skyAO        = lerp(ao, ddgiAO, skyDirCov);   // skyVisDiff: above the probe loop
        const float3 ddgiDiffPart = ddgiIrradiance * ddgiDiffuseScale * ddgiCoverage * ddgiAO;
        const float3 skyDiffPart  = skyDiff
                                  * saturate(1.0 - ddgiCoverage) * skyAO * skyVisDiff * iblStrength;
        const float3 farDiffuse   = ddgiDiffPart + skyDiffPart;
        // SSGI: Lighting.ps's hemisphere partition, transferred from the opaque surface
        // behind this fragment under the same proximity x coplanar gate as the screen AO.
        // The screen march integrated the opaque surface's hemisphere around its normal,
        // which is this fragment's only when the two coincide (a decal-like pane on a wall);
        // scaling the resolved fraction by the gate makes an ungated fragment reproduce the
        // far-field chain exactly - the kill-switch property Lighting.ps guarantees.
        // ssgiNear is deliberately not multiplied by ao (the march is the near-field
        // occlusion), as in Lighting.ps.
        float3 ssgiNear = float3(0, 0, 0);
        float  ssgiCov  = 0.0;
        if (behindGiLive)
        {
            const float4 ssgi = behindGi;
            ssgiCov  = saturate(ssgi.a) * (!BindlessNone(gRestirGiIdx) ? 1.0 : saturate(ssgiIntensity)) * aoProx;
            ssgiNear = ssgi.rgb * ddgiDiffuseScale * (!BindlessNone(gRestirGiIdx) ? 1.0 : saturate(ssgiIntensity)) * aoProx;
        }
        // Flat environment ambient (WorldEnvironment None's fill; FlatColor's is the uniform
        // sky cube): the uniform environment Lighting.ps weights the same way - diffuse
        // through kDibl, reflection through the split-sum with the fallback's specular
        // occlusion, the film's own reflection above.
        const float3 flatSpec = IndirectSpecular_ApplyEnvBRDF(flatAmbientL * specOcc, F0, envBRDF)
                              * coatIblAtten + coatFlatSpec;
        iblDiffuseTerm  = kDibl * baseColorRGB * (ssgiNear + farDiffuse * (1.0 - ssgiCov) + flatAmbientL * ao);
        dbgDdgiDiff = ddgiDiffPart; dbgSkyDiff = skyDiffPart;
        dbgDdgiCov = ddgiCoverage; dbgSkyVis = specSkyVisibility; dbgSsgiCov = ssgiCov;
        // Specular occlusion already sits inside fallbackRadiance / coatRad
        // (fallback tier only) - nothing more to multiply here.
        iblSpecularTerm = specIBL + flatSpec;
        if (isToon)
        {
            // The toon composite's scales (Lighting.ps): nprAmbientLightScale on
            // the ambient, nprSpecularLightScale on the reflection - which
            // reflectance 0 switches off entirely, the authored "no PBR
            // specular" the split-sum's bias term would otherwise leak.
            iblDiffuseTerm  *= toon.ambientScale;
            iblSpecularTerm *= (s.reflectance > 1e-4) ? toon.specularScale : 0.0;
        }
    }

    bodyColor = directDiffuse + iblDiffuseTerm;
    surfColor = directSpecular + iblSpecularTerm + s.emissive;

#if MS_TRACED_REFL_FOLD
    // DX12_FWDREFL_DEBUG=<mode> (the renderer flips gFwdReflStrength negative) paints instead of
    // shading, on translucent surfaces only:
    //   1  the fold: red = folded weight; green = a traced signal existed here but the coplanar /
    //      proximity gate rejected it; black = RTR traced nothing behind us; blue = the
    //      surface's own screen-space march confidence.
    //   2  the folded traced radiance x its total weight - the mirrored scene the surface
    //      receives, before Fresnel scales it down to a few percent.
    //   3  the front-layer tier alone: red = its weight (RTR#2 conf x strength, 0 where this
    //      fragment is not the stamped front layer).
    //   4  the front-layer radiance x weight (the pane's own RayQuery trace, before Fresnel).
    //   5  the indirect diffuse radiance (DDGI part + sky part), the number a deferred
    //      neighbour's iblDiffuse should agree with.
    //   6  CSM diagnostics (below).
    if (gFwdReflStrength <= -6.0)
    {
        bodyColor = float3(0, 0, 0);
        // CSM diagnostics: red = the RAW cascade term re-evaluated here,
        // green = cascade-0 projection in range (uv and z inside [0,1]),
        // blue = LightCB.shadowStrength (0 => ComputeShadowFactor returns 1).
        {
            const float raw = ComputeCascadeShadowTerm(worldPos, N, p.svPosition.xy);
            float4 spd = mul(float4(worldPos, 1.0), shadowMatrix[0]);
            spd.xyz /= spd.w;
            const float2 uvd = spd.xy * float2(0.5, -0.5) + 0.5;
            const bool inRange = !(uvd.x < 0.0 || uvd.x > 1.0 || uvd.y < 0.0 || uvd.y > 1.0
                                   || spd.z < 0.0 || spd.z > 1.0);
            surfColor = float3(raw, inRange ? 1.0 : 0.0, saturate(shadowStrength));
        }
    }
    else if (gFwdReflStrength <= -5.0)
    {
        bodyColor = float3(0, 0, 0);
        surfColor = dbgDdgiDiff + dbgSkyDiff;
    }
    else if (gFwdReflStrength <= -4.0)
    {
        bodyColor = float3(0, 0, 0);
        surfColor = frontRad * frontW;
    }
    else if (gFwdReflStrength <= -3.0)
    {
        // Constant magenta floor (r,b = 0.15, g = 0) on EVERY forward fragment
        // so a capture can mask "drawn by the forward path" exactly, even
        // where no trace covers the surface; red rises with the weight.
        bodyColor = float3(0, 0, 0);
        surfColor = float3(frontW * 4.0 + 0.15, 0.0, 0.15);
    }
    else if (gFwdReflStrength <= -2.0)
    {
        bodyColor = float3(0, 0, 0);
        surfColor = tracedRadiance * tracedW;
    }
    else if (gFwdReflStrength < 0.0)
    {
        bodyColor = float3(0, 0, 0);
        surfColor = float3(reuseW * 4.0,
                           (tracedConf > 0.0 ? 0.6 : 0.0) * (1.0 - reuseW),
                           marchConf * 4.0);
    }
#endif
}

// ==== Water variant: forward shade + GBuffer stamp (4 MRTs) ====
// Drawn by WaterSurfacePass in the dedicated post-Skybox slot with depth GREATER_EQUAL +
// depth write. It writes the same GBuffer1/2/3 channels the deferred path does, so the
// screen-space reflection chain traces off the water surface this frame, plus the
// alpha-blended HDR colour.
//
// The hook owns the look; the engine owns MRT packing, velocity, material-index encoding,
// the shoreline clip and the view-mode overrides.
#if MS_IS_WATER

#include "water_common.hlsli"

struct WaterOut
{
    float4 normal   : SV_TARGET0;  // GBuffer1 - N*0.5+0.5, .a = encoded matIdx
    float4 surface  : SV_TARGET1;  // GBuffer2 - (rough, metal, ao, reflectance)
    float2 velocity : SV_TARGET2;  // GBuffer3 - raw NDC delta
    float4 color    : SV_TARGET3;  // HDR scene colour, alpha-blended
};

#if DOUBLE_SIDED
WaterOut main(MaterialSurfaceVaryings i, bool isFrontFace : SV_IsFrontFace)
#else
WaterOut main(MaterialSurfaceVaryings i)
#endif
{
    DEBUG_SHADE_INVOCATION(i.sv);            // overdraw
    DEBUG_SHADE_COST(i.sv, materialIndex);   // shadingCost
#if DOUBLE_SIDED
    const bool frontFace = isFrontFace;
#else
    const bool frontFace = true;
#endif

    MaterialPixelParams p = MS_MakeParams(i, frontFace);
    SurfaceData s = MS_DefaultSurface(g_Materials[materialIndex], p);
    EvaluateMaterial(p, s);
    MS_ApplyDissolve(i, s);

    // Shoreline: fully faded pixels are discarded so they neither stamp depth nor overwrite
    // the GBuffer; the terrain/ground stays authoritative at the waterline. Same threshold
    // as WaterPass's clip(alpha - 0.02).
    clip(s.opacity - 0.02);
    DEBUG_SHADE_DENSITY(g_Materials[materialIndex], i.sv, p.uv0);   // mipDensity

    WaterOut o;
    o.velocity = MS_Velocity(i.curClip, i.prevClip);

    // Global view-mode overrides - the forward main does this too, and water must behave
    // like every other geometry pass in Wireframe/Unlit.
    if (viewMode == VIEW_MODE_WIREFRAME)
    {
        o.normal  = float4(0, 0, 0, 0);
        o.surface = float4(1, 0, 1, 0);
        o.color   = float4(WIREFRAME_COLOR, 1.0);
        return o;
    }

    o.normal  = float4(normalize(s.normalWS) * 0.5 + 0.5,
                       MS_EncodeMatIdx(materialIndex));
    o.surface = float4(saturate(s.roughness), saturate(s.metallic),
                       saturate(s.ao), s.reflectance);

    const float waterAlpha = saturate(s.opacity);

    if (viewMode == VIEW_MODE_UNLIT)
    {
        o.color = float4(s.baseColor, waterAlpha);
        return o;
    }

    // ---- Lighting: the same forward BRDF every other surface uses ----
    // Water is not a special lighting model. The hook filled SurfaceData like any PBR
    // material (baseColor = the body colour it wants seen, normalWS = the flow/ripple
    // normal, roughness/reflectance = how sharp and how Fresnel-y), and the engine derives
    // sun + IBL + probes from that.
    //
    // So there is no water-specific sun-glint or sky-reflection helper: the specular lobe of
    // the shared BRDF against gRadiance is the reflection, and its highlight against the sun
    // is the glint. Do not hand-roll them in this main.
    float3 bodyColor, surfColor;
    // Sun visibility for the water surface (WaterPass's Water.ps.hlsl has the same term):
    // without it a shadow cast across a pond does not appear, and the sun glint keeps
    // burning inside it.
    MS_ForwardLighting(p, s, i.worldPos,
                       ComputeShadowFactor(i.worldPos, s.normalWS, i.sv.xy),
                       bodyColor, surfColor);

    // Clustered point/spot lights, specular only (WaterLocalLights): MS_ForwardLighting's
    // own cluster loop is compiled out for water (!MS_IS_WATER), so without this a lit scene
    // has a dead black pool.
    surfColor += WaterLocalLights(p, s);

    // Same body/surface split as the translucent main: alpha fades the body
    // (light coming through the water) but not the surface term (reflections and
    // highlights), so a shoreline fading out keeps its glints instead of
    // dissolving. WaterSurfacePass::BuildPSODesc selects the matching RT3 blend.
#if (defined(ADDITIVE_BLEND) && ADDITIVE_BLEND) || \
    (defined(PREMULTIPLIED_BLEND) && PREMULTIPLIED_BLEND)
    o.color = float4((bodyColor + surfColor) * waterAlpha, waterAlpha);
#else
    o.color = float4(bodyColor * waterAlpha + surfColor, waterAlpha);
#endif
    return o;
}

// ==== Translucent front-layer variant: depth-writing data stamp (3 MRTs, no colour) ====
// Drawn by TranslucentFrontLayerPass into its own targets (never the GBuffer):
//   RT0 TranslucentFront_Normal   N*0.5+0.5, .a = encoded matIdx (GBuffer1 shape)
//   RT1 TranslucentFront_Surface  (rough, metal, ao, reflectance) (GBuffer2 shape)
//   RT2 TranslucentFront_Velocity raw NDC delta                     (GBuffer3 shape)
// plus depth GREATER_EQUAL + write into TranslucentFront_Depth, cleared to sky (0) each
// frame, so the hardware test keeps the nearest translucent layer. Occlusion by the opaque
// scene is a manual test against the SSRTraceDepth snapshot (bindless,
// LightCB.fwdReflDepthIdx): the engine creates no read-only DSV, and seeding this target
// from the opaque depth would hand RTReflectionPass's second instance every opaque pixel to
// trace. The same hook / dissolve / roughness floor as the colour draw, so the normal and
// roughness the trace uses are the ones the fold is applied to.
#elif MS_IS_FRONTLAYER

struct FrontLayerOut
{
    float4 normal   : SV_TARGET0;
    float4 surface  : SV_TARGET1;
    float2 velocity : SV_TARGET2;
};

#if DOUBLE_SIDED
FrontLayerOut main(MaterialSurfaceVaryings i, bool isFrontFace : SV_IsFrontFace)
#else
FrontLayerOut main(MaterialSurfaceVaryings i)
#endif
{
#if DOUBLE_SIDED
    const bool frontFace = isFrontFace;
#else
    const bool frontFace = true;
#endif
    MaterialGPUData mat = g_Materials[materialIndex];

    // Behind the opaque scene -> not a layer. Reverse-Z: nearer is LARGER, so a
    // fragment at or below the opaque depth is occluded. Unbound index (0) =
    // no opaque test (the pass is only active with the fold on, which needs
    // this index, so it is a defensive default, not a mode).
    if (!BindlessNone(gFwdReflDepthIdx))
    {
        Texture2D<float> opaqueDepth = BindlessTex2D<float>(gFwdReflDepthIdx);
        if (i.sv.z <= opaqueDepth.Load(int3(int2(i.sv.xy), 0))) discard;
    }

    MaterialPixelParams p = MS_MakeParams(i, frontFace);
    SurfaceData s = MS_DefaultSurface(mat, p);
    EvaluateMaterial(p, s);
    MS_ApplyDissolve(i, s);

    // Unlit surfaces never fold a reflection (their main returns before the
    // BRDF), and a fully transparent texel is not a layer -- neither may claim
    // the front-layer slot from the glass behind it.
    if (mat.customShadingModel == SHADING_MODEL_UNLIT) discard;
    clip(s.opacity - (1.0 / 255.0));

    FrontLayerOut o;
    o.normal   = float4(normalize(s.normalWS) * 0.5 + 0.5,
                        MS_EncodeMatIdx(materialIndex));
    o.surface  = float4(saturate(s.roughness), saturate(s.metallic),
                        saturate(s.ao), s.reflectance);
    o.velocity = MS_Velocity(i.curClip, i.prevClip);
    return o;
}

// ==== Transmission prepass: what the background behind a Transmissive surface becomes ====
// Drawn by TransmissionPrepass into three targets TransmissionApply reads before the colour pass
// draws the surface's own light (TRANSMISSIVE_BLEND below). Every blend is commutative, so the
// layers need no sort:
//   RT0 (ADD)      .xy the screen-UV offset to the refracted background, .z the angular spread
//                  of the rough lobe (radians), .w 1 per layer
//   RT1 (MULTIPLY) .rgb the share of the background that comes through
//   RT2 (MAX)      the nearest layer's reverse-Z depth
// The opaque scene occludes through the read-only GBuffer depth test.
#elif MS_IS_TRANSMISSION

struct TransmissionOut
{
    float4 offsetSpread : SV_TARGET0;
    float4 tint         : SV_TARGET1;
    float  depth        : SV_TARGET2;
};

// The screen UV of a world position through this frame's view-projection (jittered, like the
// rasterised pixel the offset starts from).
float2 MS_ScreenUV(float3 worldPos)
{
    const float4 clip = mul(float4(worldPos, 1.0), viewProj);
    const float2 ndc  = clip.xy / max(clip.w, 1e-6);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

#if DOUBLE_SIDED
TransmissionOut main(MaterialSurfaceVaryings i, bool isFrontFace : SV_IsFrontFace)
#else
TransmissionOut main(MaterialSurfaceVaryings i)
#endif
{
#if DOUBLE_SIDED
    const bool frontFace = isFrontFace;
#else
    const bool frontFace = true;
#endif
    MaterialGPUData mat = g_Materials[materialIndex];
    MaterialPixelParams p = MS_MakeParams(i, frontFace);
    SurfaceData s = MS_DefaultSurface(mat, p);
    EvaluateMaterial(p, s);
    MS_ApplyDissolve(i, s);

    // Unlit has no dielectric base to cross; a clear texel is no layer.
    if (mat.customShadingModel == SHADING_MODEL_UNLIT) discard;
    const float coverage = saturate(s.opacity);
    clip(coverage - 1.0 / 255.0);

    const float3 N     = normalize(s.normalWS);
    const float3 V     = normalize(gCameraPosWS.xyz - i.worldPos);
    const float  NdotV = saturate(dot(N, V));
    const float  eta   = max(mat.ior, 1.0);
    const float  rough = max(s.roughness, 0.045);   // the split sum's floor, as in the colour pass
    // The lobe's width above the forward floor (material_surface.hlsli's 0.045): a smooth pane does
    // not blur; specular AA's widening on a curved surface does.
    const float  alpha = max(saturate(s.roughness) * saturate(s.roughness) - 0.045 * 0.045, 0.0);
    // The share the split sum reflects, as the colour pass's IBL sees it; the rest crosses,
    // tinted once by baseColor on its way through the whole object.
    const float2 envBRDF   = EnvBRDFApprox(rough, NdotV);
    const float  reflected = saturate(MS_TransmissiveF0(mat, s) * envBRDF.x + envBRDF.y);
    const float  share     = saturate(s.transmission) * (1.0 - saturate(s.metallic));
    float3 through = (1.0 - reflected) * saturate(s.baseColor);
    float2 offset  = float2(0.0, 0.0);
    float  spread  = 2.0 * alpha;   // a thin wall: the reflection lobe mirrored through
    if (s.thickness > 0.0)
    {
        // A solid: refract in, travel `thickness` (glTF's distance through the volume, as the
        // Khronos sample viewer reads it) and take the background seen where the ray leaves;
        // the far face's Fresnel at normal incidence and the absorption along the way.
        const float3 r = refract(-V, N, 1.0 / eta);
        offset   = MS_ScreenUV(i.worldPos + r * s.thickness) - MS_ScreenUV(i.worldPos);
        through *= 1.0 - DielectricIorToF0(eta);
        through *= DielectricTransmittance(
            DielectricAttenuationCoefficient(mat.attenuationColor, mat.attenuationDistance), s.thickness);
        spread   = 2.0 * alpha * (eta - 1.0);
    }

    TransmissionOut o;
    o.offsetSpread = float4(offset * coverage, spread * coverage * share, 1.0);
    o.tint         = float4(lerp(float3(1.0, 1.0, 1.0), share * through, coverage), 1.0);
    o.depth        = i.sv.z;
    return o;
}

#else  // !MS_IS_WATER && !MS_IS_FRONTLAYER && !MS_IS_TRANSMISSION -- the ordinary forward (translucent) main

// The height fog and froxel fog the opaque scene got from its fullscreen composites, which
// ran before this pass. Applied to the value each exit below returns, per this PSO's blend
// state (the permutation's blend bits). Water is not here: it draws before the fog passes
// and is fogged by them like the opaque scene.
#include "fog_apply.hlsli"

float4 MS_ForwardFog(float4 c, float4 svPos)
{
    const FogSample f = Fog_Evaluate(svPos);
#if ADDITIVE_BLEND
    return Fog_ApplyAdditive(c, f);         // ONE / ONE
#elif MULTIPLY_BLEND
    return Fog_ApplyMultiply(c, f);         // DEST_COLOR / INV_SRC_ALPHA
#elif PREMULTIPLIED_BLEND || TRANSMISSIVE_BLEND
    return Fog_ApplyPremultiplied(c, f);    // ONE / INV_SRC_ALPHA
#else
    return Fog_ApplyAlpha(c, f);            // SRC_ALPHA / INV_SRC_ALPHA
#endif
}

#if DOUBLE_SIDED
float4 main(MaterialSurfaceVaryings i, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
#else
float4 main(MaterialSurfaceVaryings i) : SV_TARGET
#endif
{
    DEBUG_SHADE_INVOCATION(i.sv);            // overdraw
    DEBUG_SHADE_COST(i.sv, materialIndex);   // shadingCost
#if UNLIT
    // Unlit billboard: base texture straight through, no PBR.
    float4 texColor = g_BaseColor.Sample(
        g_LinearWrap, MaterialUV(g_Materials[materialIndex], i.uv));
    clip(texColor.a - 0.01);
    return texColor;
#endif

    MaterialGPUData mat = g_Materials[materialIndex];
    // Tiled UV for the two early-out paths below, which sample the base map
    // directly instead of going through MS_MakeParams/the hook.
    const float2 matUV = MaterialUV(mat, i.uv);

    // Global view-mode override (forward path produces final color directly).
    if (viewMode == VIEW_MODE_WIREFRAME)
        return float4(WIREFRAME_COLOR, 1.0);
    if (viewMode == VIEW_MODE_UNLIT)
    {
        float4 unlit = mat.baseColor * g_BaseColor.Sample(g_LinearWrap, matUV);
        clip(unlit.a - 0.01);
#if TRANSMISSIVE_BLEND
        return float4(unlit.rgb * unlit.a, unlit.a);   // ONE / INV_SRC_ALPHA
#else
        return unlit;
#endif
    }

    // DOUBLE_SIDED forward draws are CullMode NONE: flip N/BT on back faces, as the deferred
    // main does.
#if DOUBLE_SIDED
    const bool frontFace = isFrontFace;
#else
    const bool frontFace = true;
#endif

    // ---- Material evaluation via the hook ----
    MaterialPixelParams p = MS_MakeParams(i, frontFace);
    SurfaceData s = MS_DefaultSurface(mat, p);
    EvaluateMaterial(p, s);

    // Per-instance dissolve (clip + edge glow into s.emissive).
    MS_ApplyDissolve(i, s);
    DEBUG_SHADE_DENSITY(mat, i.sv, p.uv0);   // mipDensity

    // ---- Unlit shading model: the VFX "material domain" output contract ----
    // The hook fills s.emissive + s.opacity and the engine does the rest: skip BRDF/IBL
    // entirely and pack per the material's blend mode. Uniform branch on per-material data,
    // no extra PSO permutation. Additive/premultiplied multiply by opacity so Opacity fades
    // the whole effect out (trail/particle convention); straight alpha lets the blend state
    // do it.
    [branch]
    if (mat.customShadingModel == SHADING_MODEL_UNLIT)
    {
        const float  unlitA   = saturate(s.opacity);

        // ---- Exposure protection (MaterialComponent::exposureCompensation) ----
        // This branch composites after the NPRLitSnapshot copy, so it is not in the `lit`
        // image NPRStabilize derives its correction from and never receives that pass's
        // counter-scale: the multiply has to happen here. It is not double-applied for the
        // same reason: the draw is outside `lit` by construction, and the pass passes
        // overlay deltas through untouched.
        //
        // 1.0 exactly cancels ToneMap's `color *= exposure`, so the authored color survives
        // adaptation unchanged - the same full range NPRStabilize gives deferred Unlit and
        // Toon. No 0.5 clamp here on purpose: a second hardcoded literal would silently
        // desync from NPRStabilizePass::Execute's exposureProtectionLimit.
#if MULTIPLY_BLEND
        // MULTIPLY is exempt: its rgb is a FACTOR, not radiance
        // (dst' = dst * lerp(1, rgb, a)). The background it multiplies already
        // carries its own material's protection through NPRStabilize, so
        // scaling the factor would double-apply it and shift the tint.
        const float3 unlitRgb = s.emissive;
#else
        float unlitExposure = 1.0;
        if (!BindlessNone(exposureBufIdx))
        {
            StructuredBuffer<float> expBuf = BindlessSBuf<float>(exposureBufIdx);
            const float e = expBuf[0];
            if (isfinite(e) && e >= 1e-4) unlitExposure = e;
        }
        const float3 unlitRgb = s.emissive *
            lerp(1.0, rcp(unlitExposure), saturate(mat.exposureCompensation));
#endif
#if ADDITIVE_BLEND
        return MS_ForwardFog(float4(unlitRgb * unlitA, unlitA), i.sv);
#elif MULTIPLY_BLEND
        // Multiply state is (DEST_COLOR, INV_SRC_ALPHA): dst' = dst*(src + 1 - a).
        // Premultiplying makes that dst*lerp(1, rgb, a): opacity 0 leaves the scene
        // untouched (straight rgb would brighten it by dst*rgb as a -> 0).
        return MS_ForwardFog(float4(unlitRgb * unlitA, unlitA), i.sv);
#elif PREMULTIPLIED_BLEND || TRANSMISSIVE_BLEND
        return MS_ForwardFog(float4(unlitRgb * unlitA, unlitA), i.sv);
#else
        return MS_ForwardFog(float4(unlitRgb, unlitA), i.sv);
#endif
    }

    // Saturated, as the unlit and water paths are: a hook's opacity can exceed 1
    // (SchoolNight's M_Glass adds a Fresnel ramp and a texture term), and the (SRC_ALPHA,
    // INV_SRC_ALPHA) blend then scales what is behind the pane by 1 - a < 0: negative HDR.
    const float surfaceAlpha = saturate(s.opacity);

    // Same BRDF every forward surface uses (MS_ForwardLighting), with the CSM sun shadow:
    // glass, foliage cards, lit VFX and every other blended material are shadowed like
    // opaque ones.
#if TRANSMISSIVE_BLEND
    // The reflection's F0 follows the IOR (a thin wall's both faces), so the reflected and the
    // refracted light come from one interface.
    s.reflectance = MS_TransmissiveReflectance(mat, s);
#endif
    float3 bodyColor, surfColor;
    MS_ForwardLighting(p, s, i.worldPos,
                       ComputeShadowFactor(i.worldPos, s.normalWS, i.sv.xy),
                       bodyColor, surfColor);

    // ---- Glass-split premultiplied output (identical to Transparent.ps) ----
    // ...then fogged at this surface's depth (MS_ForwardFog, per blend state).
#if ADDITIVE_BLEND
    return MS_ForwardFog(float4(bodyColor + surfColor, surfaceAlpha), i.sv);
#elif MULTIPLY_BLEND
    return MS_ForwardFog(float4(bodyColor + surfColor, surfaceAlpha), i.sv);
#elif TRANSMISSIVE_BLEND
    // The background under this surface is already refracted and tinted (TransmissionApply);
    // what is left is the surface's own light, scaled by coverage: the reflection, the diffuse
    // share that does not refract, the emission. ONE / INV_SRC_ALPHA: alpha 0 when the apply
    // ran this frame, else a grey stand-in for its tint (a preview, the pass switched off).
    const float  share     = saturate(s.transmission) * (1.0 - saturate(s.metallic));
    const float3 surface   = (bodyColor * (1.0 - saturate(s.transmission)) + surfColor) * surfaceAlpha;
    const float  fallbackA = surfaceAlpha * (1.0 - share * dot(saturate(s.baseColor),
                                                              float3(0.2126, 0.7152, 0.0722)));
    return MS_ForwardFog(float4(surface, gTransmissionApplied != 0u ? 0.0 : fallbackA), i.sv);
#else
    float3 premultRgb = bodyColor * surfaceAlpha + surfColor;
    #if PREMULTIPLIED_BLEND
        return MS_ForwardFog(float4(premultRgb, surfaceAlpha), i.sv);
    #else
        return MS_ForwardFog(float4(premultRgb / max(surfaceAlpha, 1e-3), surfaceAlpha), i.sv);
    #endif
#endif
}

#endif // MS_IS_WATER / MS_IS_FRONTLAYER / MS_IS_TRANSMISSION / forward

// ==== Deferred GBuffer variant (opaque): fills the 6 MRTs from SurfaceData ====
#else // !MS_IS_FORWARD

#if defined(SHADOW_HOOK) && SHADOW_HOOK
// ==== Shadow variant: depth only ====
// The material's own hook decides coverage exactly as the GBuffer main below does, so a
// masked custom material casts the silhouette it draws (Shadow.ps sees only the engine's
// texture slots). Drawn by the shadow passes' per-draw loops with a VS that outputs these
// varyings in the shadow view (custom_vertex.hlsli under CUSTOM_VS_SHADOW + SHADOW_HOOK); b1
// is then the ShadowPerViewCB (material_bindings.hlsli), whose time and camera sit at the
// GBuffer layout's offsets, so the hook's p.viewDirWS is the main camera's, as for a vertex
// hook in a shadow pass. No outputs: the pixel stage only clips, and DXC drops whatever of
// the hook does not feed the coverage.
void main(MaterialSurfaceVaryings i)
{
    MaterialPixelParams p = MS_MakeParams(i, true);   // no DOUBLE_SIDED in a shadow PSO
    SurfaceData s = MS_DefaultSurface(g_Materials[materialIndex], p);
    EvaluateMaterial(p, s);

#if ALPHA_TEST
    {
        float a = AC_ApplyOpacityMask(s.opacity,
                                      g_Materials[materialIndex].textureHandleIds[MAT_TEX_OPACITYMAP], p.uv0);
        [branch]
        if (g_Materials[materialIndex].materialFlags & MAT_FLAG_DITHERED_COVERAGE)
        {
            // BlendMode::Dithered: a STATIC screen door, as Shadow.ps draws one -
            // a shadow map is never temporally filtered, so the GBuffer's
            // per-frame phase would crawl here.
            clip(a - StaticDither(uint2(i.sv.xy)));
        }
        else
        {
            AC_ClipGolus(a, g_Materials[materialIndex].alphaRef);
        }
    }
#endif

    // Per-instance dissolve: its clip is coverage (the edge glow is colour).
    MS_ApplyDissolve(i, s);
}

#else // !SHADOW_HOOK

#if DOUBLE_SIDED
GBufferOut main(MaterialSurfaceVaryings i, bool isFrontFace : SV_IsFrontFace)
#else
GBufferOut main(MaterialSurfaceVaryings i)
#endif
{
    DEBUG_SHADE_INVOCATION(i.sv);            // overdraw
    DEBUG_SHADE_COST(i.sv, materialIndex);   // shadingCost
#if DOUBLE_SIDED
    bool frontFace = isFrontFace;
#else
    bool frontFace = true;
#endif

    MaterialPixelParams p = MS_MakeParams(i, frontFace);
    SurfaceData s = MS_DefaultSurface(g_Materials[materialIndex], p);
    EvaluateMaterial(p, s);

#if ALPHA_TEST
    // Ben Golus anti-aliased alpha test: rescale opacity so the cutoff sits at
    // 0.5 with a one-pixel ramp (TAA jitter integrates it into a soft edge).
    {
        // The coverage rule lives in alpha_coverage.hlsli, shared with the shadow variant
        // above and with Shadow.ps: the Masked-mode opacity mask (MAT_TEX_OPACITYMAP, .r;
        // unbound = 1) multiplies s.opacity, then the dither or the Golus ramp.
        float a = AC_ApplyOpacityMask(s.opacity,
                                      g_Materials[materialIndex].textureHandleIds[MAT_TEX_OPACITYMAP], p.uv0);
        [branch]
        if (g_Materials[materialIndex].materialFlags & MAT_FLAG_DITHERED_COVERAGE)
        {
            // BlendMode::Dithered - stochastic transparency. Survive the clip with
            // probability `a`: TAA / DLSS integrate the per-frame pattern into `a` x surface
            // + (1-a) x whatever is behind, the transmission the forward Alpha route
            // composites, but every surviving pixel is a real deferred pixel that gets
            // RTReflectionPass / SSR / DDGI / RTAO / shadows on its own normal. Coverage `a`
            // is the same baseColor.a x texture alpha x OPACITYMAP.r the Masked path clips.
            clip(a - TemporalDither(uint2(i.sv.xy), gFrameIndex));
            // Reflection compensation. A coverage-dithered surface shows its specular
            // reflection on only `a` of its pixels, so after TAA the reflection would be `a`
            // x too weak, while a forward Alpha surface keeps its highlight at full strength
            // and only the body fades (the premultiplied split). Restore that by boosting F0
            // = 0.16 x reflectance^2 by 1/a for dielectrics; the clamp at reflectance 1 (F0
            // 0.16) is the physical ceiling. Metals carry F0 in the albedo and are left
            // alone; emissive is left at coverage (a 1/a boost there is exactly the
            // sparse-firefly input a TAA clamp rejects).
            s.reflectance = saturate(s.reflectance * rsqrt(max(a, 1e-3)));
            s.opacity = 1.0;
        }
        else
        {
            s.opacity = AC_ClipGolus(a, g_Materials[materialIndex].alphaRef);
        }
    }
#endif

    // Per-instance dissolve (clip + edge glow into s.emissive -> SV_TARGET5).
    MS_ApplyDissolve(i, s);
    DEBUG_SHADE_DENSITY(g_Materials[materialIndex], i.sv, p.uv0);   // mipDensity

    float2 velocity = MS_Velocity(i.curClip, i.prevClip);
    return MS_EncodeGBuffer(s, materialIndex, velocity);
}

#endif // SHADOW_HOOK
#endif // MS_IS_FORWARD

// ==== Water service layer, for every permutation ====
// The water main above already included this (its real helpers depend on the MS_IS_FORWARD
// declarations and must precede main); the include guard makes this second include a no-op
// there.
//
// Every other permutation - the deferred GBuffer one, the other forward ones, SHADOW_HOOK -
// still parses a water material's hook, which calls WaterSceneDepthBelow() etc.; without the
// stubs that variant fails to compile, is negative-cached, and the material does not draw
// there.
//
// Cost when unused: zero - the stubs are trivial and DXC strips them.
#include "water_common.hlsli"

#endif // CUSTOM_SURFACE_HLSLI
