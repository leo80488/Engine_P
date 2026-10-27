// Minimal lit forward pixel shader for reflection probe baking. Pairs with GBuffer.vs.hlsl
// unchanged; reads the sun direction + colour from the per-face capture CB at b1 (which also
// holds the face viewProj the VS consumes) and writes one HDR colour RTV for the prefilter
// compute shader downstream.
//
// Intentionally simple:
//   * Lambert direct sun, normalized by 1/PI exactly like Lighting.ps and DDGIRayTrace.cs
//   * Sky SH ambient through the shared sky_sh.hlsli evaluator (t19 space0, filled by
//     SkyIBLPass), never a local fork
//   * CSM sun shadowing (SampleSunShadow) from the main camera's cascades
//   * Normal mapping through the interpolated TBN
//   * No clustered point/spot lights
//   * No specular / metalness / emissive: every surface bakes as a Lambert diffuse
//     response off baseColor
//
// Output is HDR linear so the GGX prefilter can mip-average it without colour bleed from a
// tonemapped intermediate. Baked probes persist across SaveWorld: re-bake them after
// changing this shader.

#include "material.hlsli"
// Shared sky-SH evaluator. This MUST be the shared header, not a local copy:
// SkySHProjection.cs stores a RAW radiance projection, and only sky_sh.hlsli's
// EvalSH2 applies the Ramamoorthi clamped-cosine convolution (c0/c1/c2) that
// turns it into E(N)/PI. A fork that drops those constants over-weights L1 by
// 1.5x and L2 by 4x and tints every bake toward the zenith colour. It also
// declares gSkySH at t19 space0, which is the register this pass binds.
#include "sky_sh.hlsli"

#include "core/math.hlsli"   // PI: the Lambertian normalization of the direct term

// Push constants: the same b0 the VS uses (only materialIndex matters here).
cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint prevPosInfo;
};

// The capture CB shares register b1 with PerViewCB. The first 3 matrices match
// GBuffer.vs.hlsl exactly (so the VS works unchanged); the trailing scalars are
// capture-specific. Byte offset 228 is reserved for the instance-remap heap index because
// GBuffer.vs reads gInstanceRemapIdx there from every b1 layout; cameraPos sits at 240 so a
// float never aliases the remap field (a garbage heap index would fault the bindless fetch).
cbuffer ProbeCaptureCB : register(b1, space0)
{
    float4x4 c_viewProj;
    float4x4 c_prevViewProj;
    float4x4 c_curViewProjNoJitter;
    float3   c_sunDir;     float c_pad0;   // 192
    float3   c_sunColor;   float c_pad1;   // 208
    uint     c_pad2;                       // 224 (dissolve idx slot in PerViewCB; unused)
    uint     c_instanceRemapIdx;           // 228 (read by GBuffer.vs; 0 = identity)
    uint2    c_pad4;                       // 232
    float3   c_cameraPos;  float c_pad3;   // 240
};

StructuredBuffer<MaterialGPUData> g_Materials : register(t2, space0);

// Bindless texture pool, same convention as GBuffer.ps.
Texture2D g_AllTextures[] : register(t0, space2);

// Per-draw fallback base color + normal slots when bindless isn't valid.
Texture2D<float4> g_BaseColor : register(t3, space0);
Texture2D<float4> g_NormalMap : register(t5, space0);

// gSkySH (t19 space0) is declared by sky_sh.hlsli, included above.

#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_LinearWrap SmpFixed(SMP_LINEAR_WRAP)

// Frame-global shadow + ambient controls (filled by ReflectionProbeCapturePass).
// cascadeVP mirrors LightCB.shadowMatrix (transposed) so the same row-vector
// mul + cascade-select used by the deferred pass works here unchanged.
cbuffer ProbeShadowCB : register(b2, space0)
{
    float4x4 c_cascadeVP[4];
    float4   c_cascadeSplits;
    float3   c_camPos;        float c_shadowStrength;
    float3   c_camFwd;        float c_iblStrength;
    float    c_ambientScale;
    float    c_shadowTexel;   // 1 / the CSM's runtime resolution (ShadowPass::GetShadowResolution)
    float2   c_shadowPad;
};

// CSM sun shadow map + comparison sampler (reversed-Z, GREATER_EQUAL). Bound by
// the capture pass; only sampled when c_shadowStrength > 0 (a directional light
// is casting), so an unshadowed bake never touches these.
Texture2DArray<float>  gShadowCascades : register(t9, space0);
#define gShadowSampler SmpCmp(SMP_CMP_LINEAR_BORDER_WHITE_GE)

// Lightweight CSM sample for the bake: cascade-select like shadow.hlsli, but a cheap 4-tap
// PCF (the captured cube face is only 128x128, so heavy filtering is wasted). Cascades are
// built around the main camera, so a probe outside its cascade range bakes unshadowed
// (viewZ past the far split -> 1.0).
float SampleSunShadow(float3 worldPos)
{
    if (c_shadowStrength <= 0.0) return 1.0;
    float viewZ = dot(worldPos - c_camPos, c_camFwd);
    if (viewZ > c_cascadeSplits.w) return 1.0;
    int c = 0;
    if      (viewZ > c_cascadeSplits.z) c = 3;
    else if (viewZ > c_cascadeSplits.y) c = 2;
    else if (viewZ > c_cascadeSplits.x) c = 1;

    float4 sp = mul(float4(worldPos, 1.0), c_cascadeVP[c]);
    sp.xyz /= sp.w;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || sp.z < 0.0 || sp.z > 1.0)
        return 1.0;

    // One texel of the cascade actually bound: the CSM resolution is player-tunable
    // (shadows.csmResolution), so do not hard-code it.
    const float t = c_shadowTexel;
    float s = gShadowCascades.SampleCmpLevelZero(gShadowSampler, float3(uv, (float)c), sp.z)
            + gShadowCascades.SampleCmpLevelZero(gShadowSampler, float3(uv + float2( t,  t), (float)c), sp.z)
            + gShadowCascades.SampleCmpLevelZero(gShadowSampler, float3(uv + float2(-t,  t), (float)c), sp.z)
            + gShadowCascades.SampleCmpLevelZero(gShadowSampler, float3(uv + float2( t, -t), (float)c), sp.z);
    return lerp(1.0, s * 0.25, c_shadowStrength);
}

struct PSIn
{
    float4 sv       : SV_POSITION;
    float3 worldPos : POSITIONWS;
    float2 uv       : TEXCOORD0;
    float3 wn       : NORMAL;
    float3 wt       : TANGENT;
    float3 wbt      : BINORMAL;
    float3 col      : COLOR;
    float2 uv1      : TEXCOORD3;
    float2 uv2      : TEXCOORD4;
    float2 uv3      : TEXCOORD5;
    float4 curClip  : TEXCOORD1;
    float4 prevClip : TEXCOORD2;
};

float4 main(PSIn i) : SV_TARGET
{
    MaterialGPUData mat = g_Materials[materialIndex];

    // Material UV transform, as custom_surface.hlsli's MS_MakeParams, so a tiled material bakes
    // into the probe the way it renders.
    const float2 muv = MaterialUV(mat, i.uv);

    float4 baseColor = mat.baseColor;
    if (mat.paramCount == 0)
        baseColor = float4(i.col, 1.0);  // no material params: vertex colour

    int texBaseColor = mat.textureHandleIds[0];
    if (texBaseColor >= 0)
        baseColor *= g_AllTextures[texBaseColor].Sample(g_LinearWrap, muv);
    else
        baseColor *= g_BaseColor.Sample(g_LinearWrap, muv);

    // Alpha cutout, as Shadow.ps / the GBuffer ALPHA_TEST permutation. This pass builds one PSO
    // with no ALPHA_TEST permutation; without this the probe cubemap bakes alpha-clip cards solid
    // and the RTR probe/sky fallback tier contradicts the traced tier. `baseColor` is already
    // mat.baseColor * tex (GBuffer parity). The Masked-mode opacity mask (MAT_TEX_OPACITYMAP, .r)
    // multiplies coverage, as custom_surface.hlsli's ALPHA_TEST block and Shadow.ps.
    int texMask = mat.textureHandleIds[MAT_TEX_OPACITYMAP];
    if (texMask >= 0)
        baseColor.a *= g_AllTextures[texMask].Sample(g_LinearWrap, muv).r;
    // Dithered materials bake opaque here: MaskClipValueFor publishes alphaRef = 1.0 (no clip)
    // for that mode, so a leftover authored threshold cannot punch holes into a low-frequency
    // irradiance bake.
    if (mat.alphaRef <= (1.0 - 1.0 / 256.0) && (texBaseColor >= 0 || texMask >= 0))
        clip(baseColor.a - mat.alphaRef);

    // Normal mapping (mirrors GBuffer.ps): unpack tangent-space RG, derive Z,
    // optionally scale by the material's normal-strength knob, then transform
    // through the interpolated TBN to world space. Falls back to the geometry
    // normal if the material has no normal map (texNormal < 0).
    int    texNormal      = mat.textureHandleIds[1];
    float  normalStrength = mat.normalStrength;
    if (mat.paramCount == 0) normalStrength = 1.0;

    float2 rg = (texNormal >= 0)
                ? g_AllTextures[texNormal].Sample(g_LinearWrap, muv).rg * 2.0 - 1.0
                : g_NormalMap.Sample(g_LinearWrap, muv).rg * 2.0 - 1.0;
    float  nz = sqrt(saturate(1.0 - dot(rg, rg)));
    float3 ts = float3(rg, nz);
    ts.xy *= normalStrength;
    ts = normalize(ts);

    float3 N  = normalize(i.wn);
    float3 T  = normalize(i.wt);
    float3 BT = normalize(i.wbt);
    float3x3 TBN = float3x3(T, BT, N);
    N = normalize(mul(ts, TBN));

    // Baked ambient occlusion: the surface map's R channel (R = AO, the GBuffer.ps convention).
    // Occludes the ambient term so crevices / interiors do not bake as bright as open surfaces;
    // 1.0 when there is no map.
    float ao = 1.0;
    int   texSurface = mat.textureHandleIds[2]; // SURFACEMAP
    if (texSurface >= 0)
        ao = g_AllTextures[texSurface].Sample(g_LinearWrap, muv).r;

    // Direct sun: Lambert, shadowed by the main camera's CSM so roofed interiors do not bake full
    // sunlight. c_sunDir is the world-space direction the light travels in (as LightCB.lightDir),
    // so L = normalize(-c_sunDir), as in Lighting.ps. The caller must hand a travel vector:
    // SkyIBLPass::GetSunDir() stores the toward-sun one, so Renderer::ProcessProbeBakeQueue
    // negates it.
    //
    // The 1/PI is the Lambertian normalization, matching the deferred pass (Lighting.ps
    // `kD * albedo / PI`, DDGIRayTrace `albedo * E * (1/PI)`). kD = (1-F)(1-metalness) is not
    // applied: this shader reads no metalness/roughness, so metals bake as full Lambert diffuse.
    float3 L      = normalize(-c_sunDir);
    float  NdL    = saturate(dot(N, L));
    float  shadow = SampleSunShadow(i.worldPos);
    float3 sun    = baseColor.rgb * c_sunColor * NdL * shadow * (1.0 / PI);

    // Ambient: sky SH irradiance x the live iblStrength (so the capture matches the main pass's
    // indirect level) x the artist bake-ambient knob x baked AO. EvalSH2 returns E(N)/PI, so
    // `baseColor * EvalSH2(N)` is already the normalized Lambertian response: do not divide by
    // PI again (the direct term does, because its input is radiance, not irradiance).
    float3 amb  = baseColor.rgb * EvalSH2(N) * (c_iblStrength * c_ambientScale * ao);

    // Output HDR linear, no tonemap or exposure: the prefilter needs raw radiance for physically
    // meaningful mip levels.
    return float4(sun + amb, 1.0);
}
