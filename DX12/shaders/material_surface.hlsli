// material_surface.hlsli - the pixel-side material contract of the template+hook material
// system (the pixel analogue of custom_vertex.hlsli).
// Defines the surface hook interface - SurfaceData (what a material produces) and
// MaterialPixelParams (what the engine hands the hook) - plus the engine-owned plumbing every
// material PS shares: bindless/per-draw texture sampling, normal-map unpack + TBN transform,
// geometric specular AA, NDC velocity and the GBuffer MRT encode (MS_EncodeGBuffer). A
// material only fills SurfaceData; the engine owns main, the GBuffer schema, masked clip and
// multi-pass routing (custom_surface.hlsli). The default PBR material is the weak-default
// EvaluateMaterial below; a custom material replaces that one function.
// Included by custom_surface.hlsli (and through it by GBuffer.ps / Transparent.ps and every
// custom material .ps.hlsl, which #defines MATERIAL_HAS_SURFACE before its EvaluateMaterial),
// custom_material.hlsli and OutlineGeometry.ps.
// Uses only slots the GBuffer/Transparent root signatures expose (b0 push constants, b1
// PerViewCB [VISIBILITY_ALL, carries cameraPos], t2 material SB, t3/t4/t5 per-draw fallback
// textures, t0 space2 bindless, s0 sampler); custom material params/textures live at b8 / space3.

#ifndef MATERIAL_SURFACE_HLSLI
#define MATERIAL_SURFACE_HLSLI

// All engine root-sig bindings (PushConstants, PerViewCB [cameraPos @208], the material
// buffer + per-draw/bindless textures + sampler) live in the shared material_bindings.hlsli
// so the vertex template, this pixel template and the unified custom_material.hlsli agree on
// the layout. DXC strips whatever a stage does not read.
#include "material_bindings.hlsli"

// ==== Hook interface ====
// MaterialPixelParams (hook input), SurfaceData (hook output) and the vertex hook's
// MaterialVertexParams live in material_types.hlsli so the vertex side shares the same
// structs (a unified single-file material compiles for both stages). The include guard makes
// this a no-op when custom_material.hlsli already pulled it in.
#include "material_types.hlsli"

// core/gbuffer_codec.hlsli: the GBuffer layout and matIdx codec. shading_models.hlsli: the
// RT4 codec for the non-Standard shading models (anisotropy angle, subsurface channels),
// shared verbatim with the decoder in Lighting.ps.hlsl; its header says why it lives in
// neither file.
#include "core/gbuffer_codec.hlsli"   // GBufferEncodeMatIdx
#include "shading_models.hlsli"

// ==== Engine helpers (so hooks and the weak default stay terse) ====

// Texture source by pass: the deferred GBuffer path uses bindless with a per-draw fallback
// (the ExecuteIndirect groups bind only the bindless table); the forward transparent path is
// per-draw only (no indirect path), so it samples the per-draw bound textures directly and
// avoids the bindless-or-fallback double sample.
float4 MS_SampleBaseColor(MaterialGPUData mat, float2 uv)
{
#if defined(MS_IS_FORWARD) && MS_IS_FORWARD
    return g_BaseColor.Sample(g_LinearWrap, uv);
#else
    int t = mat.textureHandleIds[0];   // BASECOLORMAP
    return (t >= 0) ? g_AllTextures[t].Sample(g_LinearWrap, uv)
                    : g_BaseColor.Sample(g_LinearWrap, uv);
#endif
}

float4 MS_SampleSurface(MaterialGPUData mat, float2 uv)
{
#if defined(MS_IS_FORWARD) && MS_IS_FORWARD
    return g_SurfaceMap.Sample(g_LinearWrap, uv);
#else
    int t = mat.textureHandleIds[2];   // SURFACEMAP: R=AO G=roughness B=metalness
    return (t >= 0) ? g_AllTextures[t].Sample(g_LinearWrap, uv)
                    : g_SurfaceMap.Sample(g_LinearWrap, uv);
#endif
}

// Tangent-space normal from the normal map (RG stored, B reconstructed). XY scaled by
// strength (Z stays derived, so the normal flattens as strength -> 0).
float3 MS_SampleNormalTS(MaterialGPUData mat, float2 uv, float strength)
{
#if defined(MS_IS_FORWARD) && MS_IS_FORWARD
    float2 rg = g_NormalMap.Sample(g_LinearWrap, uv).rg * 2.0 - 1.0;
#else
    int t = mat.textureHandleIds[1];   // NORMALMAP
    float2 rg = ((t >= 0) ? g_AllTextures[t].Sample(g_LinearWrap, uv).rg
                          : g_NormalMap.Sample(g_LinearWrap, uv).rg) * 2.0 - 1.0;
#endif
    float  nz = sqrt(saturate(1.0 - dot(rg, rg)));
    float3 ts = float3(rg, nz);
    ts.xy *= strength;
    return normalize(ts);
}

float3 MS_ApplyNormalMap(float3 tsNormal, float3x3 tangentToWorld)
{
    return normalize(mul(tsNormal, tangentToWorld));
}

float3 MS_SampleEmissive(MaterialGPUData mat, float2 uv)
{
    float3 e = mat.emissiveColor.rgb * mat.emissiveColor.w;
    int t = mat.textureHandleIds[MAT_TEX_EMISSIVEMAP];
    if (t >= 0) e *= g_AllTextures[t].Sample(g_LinearWrap, uv).rgb;
    return e;
}

// Geometric specular AA (Kaplanyan & Hable 2016): widen the GGX lobe by the
// screen-space normal variance so rough-but-not-mirror surfaces stop sparkling
// under TAA jitter. Engine constants (sigma^2=0.25, kappa=0.18). Applied by the
// weak default; custom hooks opt in by calling this.
void MS_GeometricSpecularAA(inout float roughness, float3 worldNormal)
{
    const float kSigma2 = 0.25;
    const float kKappa  = 0.18;
    float3 dndu     = ddx(worldNormal);
    float3 dndv     = ddy(worldNormal);
    float  variance = kSigma2 * (dot(dndu, dndu) + dot(dndv, dndv));
    float  kernelR2 = min(2.0 * variance, kKappa);
    float  alpha    = roughness * roughness;
    float  alpha2   = saturate(alpha + kernelR2);
    roughness       = sqrt(alpha2);
}

// NDC motion vector for TAA (both clip positions UNJITTERED).
float2 MS_Velocity(float4 curClip, float4 prevClip)
{
    return curClip.xy / curClip.w - prevClip.xy / prevClip.w;
}

// Rotates dir about axis by turns (1 = a full turn), Rodrigues. The anisotropy axis needs
// this: an artist authoring brushed metal thinks in "rotate the grain 30 deg", not in
// tangent-space vectors. axis must be unit; dir need not be.
float3 MS_RotateAboutAxis(float3 dir, float3 axis, float turns)
{
    if (turns == 0.0) return dir;
    float s, c;
    sincos(turns * 6.28318530718, s, c);
    return dir * c + cross(axis, dir) * s + axis * (dot(axis, dir) * (1.0 - c));
}

// World-space anisotropy direction from a tangent-space 2D direction - the shape a flow map /
// hair-direction map is authored in (rg = xy, usually biased so 0.5 is zero). Feed it the
// decoded sample (`tex.rg * 2 - 1`); z is dropped because only the in-plane part of the axis
// survives the GBuffer encode.
float3 MS_AnisoDirFromTangentSpace(float2 dirTS, float3x3 tangentToWorld)
{
    return mul(float3(dirTS, 0.0), tangentToWorld);
}

// SurfaceData seed so a hook only sets what it cares about. This bare form has no pixel, so
// its normal is a placeholder (world +Z); every pass seeds through the params overload below,
// which replaces it with the surface's own geometric normal.
SurfaceData MS_DefaultSurface(MaterialGPUData mat)
{
    SurfaceData s;
    s.baseColor    = mat.baseColor.rgb;
    s.opacity      = mat.baseColor.a;
    s.normalWS     = float3(0.0, 0.0, 1.0);
    s.roughness    = 0.5;
    s.metallic     = 0.0;
    s.ao           = 1.0;
    s.reflectance  = mat.reflectance;
    s.emissive     = float3(0.0, 0.0, 0.0);
    s.extra        = float4(0.0, 0.0, 0.0, 0.0);
    s.directAdditive = 0.0;
    s.shadingModel = SHADING_MODEL_STANDARD;
    // Shading-model channels, seeded from the material so "pick Anisotropic / Subsurface in
    // the Inspector and touch no shader" works with a hook that never mentions them. The X axis
    // is a placeholder the params overload below replaces with the real mesh tangent.
    s.anisotropy        = clamp(mat.anisotropy, -1.0, 1.0);
    s.anisotropyDirWS   = float3(1.0, 0.0, 0.0);
    s.subsurfaceOpacity = saturate(mat.subsurfaceOpacity);
    s.subsurfaceMask    = 1.0;
    s.clearCoat         = saturate(mat.clearCoat);
    s.clearCoatRoughness= saturate(mat.clearCoatRoughness);
    s.transmission      = saturate(mat.transmission);
    s.thickness         = max(mat.thickness, 0.0);
    return s;
}

// ==== GBuffer MRT encode ====
// The layout, GBufferOut, is core/gbuffer_codec.hlsli's, beside the matIdx codec and the
// no-material encoder terrain and grass use.

// matIdx packed for GBuffer1.a: the index and the material's XeGTAO-exclude flag (layout:
// core/gbuffer_codec.hlsli). Separate from MS_EncodeGBuffer so every pass that stamps
// GBuffer1 outside the 6-MRT deferred path uses the identical encoding: WaterSurfacePass
// writes a 4-MRT subset and must agree, or deferred lighting/XeGTAO read the wrong material
// for every water pixel.
float MS_EncodeMatIdx(uint matIdx)
{
    const bool excludeSSAO =
        (g_Materials[matIdx].materialFlags & MAT_FLAG_EXCLUDE_FROM_SSAO) != 0;
    return GBufferEncodeMatIdx(matIdx, excludeSSAO);
}

GBufferOut MS_EncodeGBuffer(SurfaceData s, uint matIdx, float2 velocity)
{
    const float matIdxEnc = MS_EncodeMatIdx(matIdx);

    GBufferOut o;
    o.albedo   = float4(s.baseColor, s.opacity);
    o.normal   = float4(normalize(s.normalWS) * 0.5 + 0.5, matIdxEnc);
    o.surface  = float4(saturate(s.roughness), saturate(s.metallic), saturate(s.ao), s.reflectance);
    o.velocity = velocity;
    // RT4.a carries the face marker on face pixels and, on non-face pixels, the stylized
    // direct-additive mask (SurfaceData::directAdditive). They never collide: the marker is
    // >= 1.0, this encoding is strictly < 0.5, and every face test is `extra.a > 0.5`. A hook
    // that writes s.extra.a itself (a face shader) wins: its value is >= 1.0 and the max()
    // below preserves it.
    o.extra    = s.extra;
    o.extra.a  = max(o.extra.a,
                     saturate(s.directAdditive) * DIRECT_ADDITIVE_ENCODE_SCALE);

    // ---- Shading-model channels (RT4.r/.g) ----
    // The engine owns this packing, not the hook: a material declares its model in the Inspector
    // and fills semantic SurfaceData fields, and the codec that must agree with the deferred
    // decoder lives in one place (shading_models.hlsli, which also has the RT4 channel budget).
    // .b and .a are untouched, so an anisotropic hair material keeps its FaceOverlay mask and
    // its stylized direct-additive.
    // Face pixels are exempt: a V2 face uses all four RT4 channels (SDF halves in .r/.b, hair
    // screen mark in .g, marker in .a) and RGBA16F has no fifth, so a mis-set dropdown must not
    // overwrite the SDF terminator with an anisotropy angle. A marked face keeps its channels and
    // the model's per-pixel half degrades to the material default. The test is the same `> 0.5`
    // every face consumer uses.
    const uint   msModel     = g_Materials[matIdx].customShadingModel;
    const bool   msFaceOwned = (s.extra.a > 0.5);
    if (!msFaceOwned)
    {
        if (msModel == SHADING_MODEL_ANISOTROPIC)
        {
            // Encode against the normal the decoder will see - o.normal.rgb through the same 16-bit
            // UNORM round trip the ROP applies - so both sides pick the same reference-frame sign by
            // construction, not by the UNORM midpoint landing on N.z == 0. SM_DequantizeGBufferNormal
            // says how small the guarded error is and why it is guarded anyway.
            o.extra.r = SM_EncodeAnisoDir(SM_DequantizeGBufferNormal(o.normal.rgb),
                                          s.anisotropyDirWS);
            o.extra.g = clamp(s.anisotropy, -1.0, 1.0);
        }
        else if (msModel == SHADING_MODEL_SUBSURFACE)
        {
            o.extra.r = saturate(s.subsurfaceOpacity);
            o.extra.g = saturate(s.subsurfaceMask);
        }
        else if (msModel == SHADING_MODEL_CLEARCOAT)
        {
            o.extra.r = saturate(s.clearCoat);
            o.extra.g = saturate(s.clearCoatRoughness);
        }
    }

    o.sceneCol = float4(s.emissive, 1.0);
    return o;
}

// ==== The engine standard-PBR surface ====
// The plain material's evaluation from its engine texture slots and schema parameters. It is
// the weak-default EvaluateMaterial below, and a material graph's Standard Surface node calls
// it (so "Convert to Graph" reproduces a plain material exactly). Always defined; DXC strips
// it from a shader that never calls it.
void MS_StandardSurface(MaterialPixelParams p, inout SurfaceData s)
{
    MaterialGPUData mat = g_Materials[p.materialIndex];

    float4 baseColor      = mat.baseColor;
    float  roughnessMin   = mat.roughnessMin;
    float  roughnessMax   = mat.roughnessMax;
    float  metalnessMin   = mat.metalnessMin;
    float  metalnessMax   = mat.metalnessMax;
    float  reflectance    = mat.reflectance;
    float  normalStrength = mat.normalStrength;

    // No material params uploaded yet -> direct vertex-color preview defaults.
    bool useFallback = (mat.paramCount == 0);
    if (useFallback)
    {
        baseColor      = float4(p.vertexColor.rgb, 1.0);
        roughnessMin   = 0.0;  roughnessMax  = 0.5;
        metalnessMin   = 0.0;  metalnessMax  = 0.0;
        reflectance    = 0.04;
        normalStrength = 1.0;
    }

    baseColor *= MS_SampleBaseColor(mat, p.uv0);

    // Opt-in per-vertex color tint (skipped on the fallback path, which already
    // folded vertex color into baseColor above).
    if (!useFallback && (mat.materialFlags & MAT_FLAG_USE_VERTEXCOLOR))
        baseColor.rgb *= p.vertexColor.rgb;

    float4 surface   = MS_SampleSurface(mat, p.uv0);
    float  ao        = surface.r;
    float  roughness = lerp(roughnessMin, roughnessMax, surface.g);
    float  metalness = lerp(metalnessMin, metalnessMax, surface.b);

    float3 tsNormal  = MS_SampleNormalTS(mat, p.uv0, normalStrength);
    float3 worldN    = MS_ApplyNormalMap(tsNormal, p.tangentToWorld);

    // Forward (translucent) shading needs a roughness floor for BRDF stability, applied before
    // specular AA. MS_IS_FORWARD is set by custom_surface.hlsli before this header is included
    // (undefined -> 0 when a deferred pass uses this header standalone).
#if defined(MS_IS_FORWARD) && MS_IS_FORWARD
    roughness = max(roughness, 0.045);
#endif

    // Geometric specular AA (default path applies it; custom hooks opt in).
    MS_GeometricSpecularAA(roughness, worldN);

    s.baseColor    = baseColor.rgb;
    s.opacity      = baseColor.a;
    s.normalWS     = worldN;
    s.roughness    = roughness;
    s.metallic     = metalness;
    s.ao           = ao;
    s.reflectance  = reflectance;
    s.emissive     = MS_SampleEmissive(mat, p.uv0);
    // extra.b = hair mask (MAT_FLAG_HAIR_MASK): lets a plain material's pixels
    // count as "hair" for the FaceOverlay brows-through-hair clip without
    // swapping its surface shader. Same RT4 contract as asset/Shaders/Hair.ps.hlsl.
    s.extra        = (mat.materialFlags & MAT_FLAG_HAIR_MASK)
                   ? float4(0.0, 0.0, 1.0, 0.0)
                   : float4(0.0, 0.0, 0.0, 0.0);
    s.directAdditive = 0.0;
    s.shadingModel = SHADING_MODEL_STANDARD;
}

// ==== The seed every pass hands a hook ====
// The bare seed above plus what only the pixel knows: the interpolated geometry normal (a
// hook that never writes s.normalWS is lit as the surface it is, not as one facing world +Z)
// and the anisotropy axis resolved from the mesh tangent and the material's rotation knob.
// MATERIAL_INHERIT_STANDARD: a material that #defines it (any value) before including
// custom_surface.hlsli starts from the engine's standard surface instead (its own texture
// slots and parameters, exactly what a plain material draws via MS_StandardSurface), and its
// hook changes only what it overrides.
SurfaceData MS_DefaultSurface(MaterialGPUData mat, MaterialPixelParams p)
{
    SurfaceData s = MS_DefaultSurface(mat);
    s.normalWS        = p.geoNormalWS;
    s.anisotropyDirWS = MS_RotateAboutAxis(p.tangentWS, p.geoNormalWS,
                                           mat.anisotropyRotation);
#ifdef MATERIAL_INHERIT_STANDARD
    MS_StandardSurface(p, s);
#endif
    return s;
}

// ==== Weak default ====
// A material that does not `#define MATERIAL_HAS_SURFACE` gets the standard surface above;
// a custom material defines its own EvaluateMaterial after that #define.
#ifndef MATERIAL_HAS_SURFACE
void EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s)
{
    MS_StandardSurface(p, s);
}
#endif // MATERIAL_HAS_SURFACE

#endif // MATERIAL_SURFACE_HLSLI
