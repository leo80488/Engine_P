// Transparent additive custom PS for the outer glow shell of a procedural-tube beam
// (MaterialComponent.customShaderPath, userBlendMode = Additive). One HDR colour, PSO blend
// ONE/ONE (permutation ADDITIVE_BLEND), premultiplied: alpha is the fade envelope.
//   color = GlowColor * Intensity * noise(uv.y) * rim(uv.x)
//   alpha = endFade(uv.y) * rim(uv.x)
// The noise is static: ScrollSpeed is multiplied by 0 because the CBV has no time.

#include "material.hlsli"
#include "fog_apply.hlsli"   // FogApplyCB b4 + s3, bound by TransparentPass

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

cbuffer BeamGlowParams : register(b8, space0)
{
    float4 GlowColor;     // .rgb HDR
    float  Intensity;
    float  RimPower;       // silhouette emphasis
    float  NoiseTiling;
    float  ScrollSpeed;
    float  NoiseFloor;
    float  HeadTailFade;   // smoothstep range at uv.y = 0 and uv.y = 1
    float  _pad0;
    float  _pad1;
};

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

float4 main(PSIn i) : SV_TARGET
{
    // ---- Rim ----
    // The glow peaks at the tube silhouette, where the view crosses the most shell depth.
    // uv.x = 0.5 is taken as the tube centre seen from the camera, 0 / 1 as the edges.
    float side = abs(i.uv.x - 0.5) * 2.0;       // 0 centre, 1 silhouette
    float rim  = pow(saturate(side), max(RimPower, 0.001));

    // ---- Noise along the beam axis ----
    float u = i.uv.y * max(NoiseTiling, 0.001) - ScrollSpeed * 0.0;
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

    // ---- Head / tail fade, so the beam does not end in a flat slice ----
    float fadeRange = max(HeadTailFade, 0.001);
    float endFade   = smoothstep(0.0, fadeRange,        i.uv.y)
                    * smoothstep(0.0, fadeRange, 1.0 -  i.uv.y);

    // ---- Compose ----
    float3 rgb   = GlowColor.rgb * Intensity * n * rim;
    float  alpha = endFade * rim;

    if (alpha < 0.002) discard;

    // Premultiply by alpha so the ONE/ONE contribution fades at the edges instead of cutting off,
    // then fog at this fragment's depth like every additive surface drawn after the fog composite.
    return Fog_ApplyAdditive(float4(rgb * alpha, alpha), Fog_Evaluate(i.sv));
}
