// Opaque GBuffer custom PS for the inner core of a procedural-tube beam
// (MaterialComponent.customShaderPath, userBlendMode = Opaque). Shading comes from the
// bindless MaterialGPUData slot plus the custom CBV the Inspector reflects:
//   BeamColor    - linear HDR core colour (1..10)
//   Intensity    - multiplier on BeamColor.rgb (1..20)
//   HotCoreColor - lerp target for the white-hot centre
//   RimPower     - rim sharpness exponent;  RimIntensity - extra emissive at the rim
//   NoiseTiling  - noise tiles along the beam (uv.y);  NoiseFloor - minimum noise multiplier
//   ScrollSpeed  - unused here (the CBV carries no time, so the noise is static)
//   Roughness    - surface roughness (IBL spec sharpness)
// Outputs: albedo = HDR glow, normal = the real tube normal, surface = roughness / metalness /
// AO / reflectance, velocity = TAA motion vector.

#include "material.hlsli"
#include "core/gbuffer_codec.hlsli"   // GBufferEncodeMatIdx

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

StructuredBuffer<MaterialGPUData> g_Materials   : register(t2, space0);
Texture2D                         g_AllTextures[] : register(t0, space2);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_LinearWrap SmpDynamic()   // the material sampler

// Tunable params; the Inspector picks them up via reflection.
cbuffer BeamCoreParams : register(b8, space0)
{
    float4 BeamColor;      // .rgb HDR core; .a unused
    float  Intensity;
    float  RimPower;
    float  RimIntensity;
    float  NoiseTiling;
    float  ScrollSpeed;
    float  NoiseFloor;
    float  Roughness;
    float  _pad0;
    float4 HotCoreColor;   // hottest centre tint, lerped in by the radial term
};

// Optional noise texture; unbound (black at the centre) -> procedural noise.
Texture2D NoiseTex : register(t0, space3);

struct PSIn
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;
    float4 curClip   : TEXCOORD1;
    float4 prevClip  : TEXCOORD2;
};

struct GOut
{
    float4 albedo   : SV_TARGET0;
    float4 normal   : SV_TARGET1;
    float4 surface  : SV_TARGET2;
    float2 velocity : SV_TARGET3;
    float4 extra    : SV_TARGET4;
    float4 sceneCol : SV_TARGET5;   // HdrSceneColor: emissive seed
};

#include "core/hash.hlsli"

float ProceduralNoise1D(float u, float scrollOffset)
{
    float fp = u + scrollOffset;
    float i  = floor(fp);
    float f  = frac(fp);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(HashHoskins12(float2(i, 7.0)), HashHoskins12(float2(i + 1.0, 7.0)), f);
}

GOut main(PSIn i)
{
    MaterialGPUData mat = g_Materials[materialIndex];

    // ---- Rim ----
    // The GBuffer PS has no camera position, so the rim is a UV proxy for 1 - |N.V|: uv.x runs
    // 0..1 around the ring, and its distance from 0.5 stands in for the silhouette.
    float side = abs(i.uv.x - 0.5) * 2.0;       // 0 centre, 1 silhouette
    float rim  = pow(saturate(side), max(RimPower, 0.001));

    // ---- Noise along the beam axis ----
    // Static: ScrollSpeed is multiplied by 0 because the CBV has no time.
    float u = i.uv.y * max(NoiseTiling, 0.001) + ScrollSpeed * 0.0;
    float n;
    if (any(NoiseTex.SampleLevel(g_LinearWrap, float2(0.5, 0.5), 0).rgb))
    {
        float2 nuv = float2(i.uv.x, u);
        n = NoiseTex.SampleLevel(g_LinearWrap, nuv, 0).r;
    }
    else
    {
        n = ProceduralNoise1D(u, 0.0);
    }
    n = lerp(saturate(NoiseFloor), 1.0, n);

    // ---- Emissive ----
    float3 baseCol = BeamColor.rgb * Intensity * n;

    // White-hot centre: lerp toward HotCoreColor where the rim term is low.
    baseCol = lerp(HotCoreColor.rgb, baseCol, saturate(side));

    // Rim emissive on top.
    baseCol += BeamColor.rgb * RimIntensity * rim;

    // ---- TAA velocity ----
    float2 curNDC  = i.curClip.xy  / i.curClip.w;
    float2 prevNDC = i.prevClip.xy / i.prevClip.w;

    // ---- Output ----
    // normal.a packs the material index for the UNORM RT1 (core/gbuffer_codec.hlsli).
    const bool excludeSSAO = (mat.materialFlags & MAT_FLAG_EXCLUDE_FROM_SSAO) != 0;
    const float matIdxEnc  = GBufferEncodeMatIdx(materialIndex, excludeSSAO);

    GOut o;
    o.albedo   = float4(baseCol, 1.0);
    o.normal   = float4(normalize(i.wn) * 0.5 + 0.5, matIdxEnc);
    o.surface  = float4(saturate(Roughness), 0.0, 1.0, 0.04);
    o.velocity = curNDC - prevNDC;
    o.extra    = float4(0, 0, 0, 0);
    // The beam is Unlit: its glow reaches the scene through albedo (Lighting.ps's Unlit variant
    // adds albedo onto sceneCol), so no emissive seed is written.
    o.sceneCol = float4(0, 0, 0, 1);
    return o;
}
