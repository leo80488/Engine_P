// Tracer pixel shader: additive cylindrical-billboard beam, HDR (blend SRC_ALPHA / ONE).
//   rgb   = color.rgb * intensity
//   alpha = coreShape(uv.x)           triangular cross-section
//         * scrollNoise(uv.y, time)   along-beam plasma
//         * softParticleFade(depth)   edge fade against geometry
//         * lifeAlpha                 age decay
// Bindings:
//   b2 space0 - TracerRenderCB (as Tracer.vs.hlsl)
//   t5 space0 - Texture2D<float> gSceneDepth (hardware depth, reverse-Z)
//   t0 space2 - bindless textures (optional noise texture instead of the procedural one)
//   b4 / s3   - FogApplyCB + the froxel sampler (fog_apply.hlsli)

#include "Tracer.hlsli"
#include "DepthCommon.hlsli"
#include "fog_apply.hlsli"

cbuffer TracerRenderCB : register(b2, space0)
{
    float4x4 viewProj;
    float3   cameraPos;
    float    time;
    float    nearZ;
    float    farZ;
    float    fadeRange;
    float    coreSharpness;
    float    noiseTiling;
    float    scrollSpeed;
    float    noiseFloor;
    float    _pad;
};

Texture2D<float>  gSceneDepth      : register(t5, space0);
Texture2D<float4> g_AllTextures[]  : register(t0, space2);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define gLinear SmpFixed(SMP_LINEAR_WRAP)

struct VSOut
{
    float4 pos       : SV_POSITION;
    float2 uv        : TEXCOORD0;
    float4 color     : COLOR0;
    float  lifeAlpha : TEXCOORD1;
    nointerpolation uint noiseIdx : TEXCOORD2;
    float  spawnTime : TEXCOORD3;
};

#include "core/hash.hlsli"

// 1-D smooth value noise along the beam (uv.y axis).
float ProceduralNoise1D(float u, float scrollOffset)
{
    float fp = u + scrollOffset;
    float i  = floor(fp);
    float f  = frac(fp);
    f = f * f * (3.0 - 2.0 * f);   // smoothstep
    float a = HashHoskins12(float2(i,       7.0));
    float b = HashHoskins12(float2(i + 1.0, 7.0));
    return lerp(a, b, f);
}

float4 main(VSOut input) : SV_TARGET
{
    // Core shape: triangular falloff across the beam (uv.x 0..1), brightest at the centre;
    // coreSharpness is the art-tunable exponent.
    float core = saturate(1.0 - abs(input.uv.x * 2.0 - 1.0));
    core = pow(core, max(coreSharpness, 0.001));

    // Noise scrolling along the beam, relative to spawnTime so each tracer's pattern is stable across
    // frames and does not pop on respawn.
    float scrollOff = (time - input.spawnTime) * scrollSpeed;
    float u         = input.uv.y * max(noiseTiling, 0.001);

    float noise;
    if (input.noiseIdx != 0xFFFFFFFFu)
    {
        // Noise texture along (uv.x, scrolled uv.y).
        float2 nuv = float2(input.uv.x, u - scrollOff);
        noise = g_AllTextures[NonUniformResourceIndex(input.noiseIdx)]
                    .SampleLevel(gLinear, nuv, 0).r;
    }
    else
    {
        noise = ProceduralNoise1D(u, -scrollOff);
    }
    noise = lerp(saturate(noiseFloor), 1.0, noise);

    // Soft-particle fade against scene depth.
    int2  px      = int2(input.pos.xy);
    float sceneZ  = gSceneDepth.Load(int3(px, 0));
    float fade    = SoftParticleFade(input.pos.z, sceneZ, nearZ, farZ, fadeRange);

    // Combine.
    float intensity = max(input.color.a, 0.0);
    float3 rgb      = input.color.rgb * intensity;
    float  alpha    = core * noise * fade * input.lifeAlpha;

    // Drop sub-pixel-dim contributions to save blend work.
    if (alpha < 0.002) discard;

    // The fog the opaque scene got at this fragment's depth: additive light is attenuated by the
    // transmittance in front of it.
    return Fog_ApplyAdditive(float4(rgb, alpha), Fog_Evaluate(input.pos));
}
