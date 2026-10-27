// Skybox.ps stripped down to "sample one cubemap with the world-space direction the VS
// produced". The reflection probe capture pass uses it to fill every pixel the opaque pass
// left untouched (the sky seen through windows / doorways), so the prefiltered cube has no
// black hole where geometry did not cover.
//
// Pairs with Skybox.vs.hlsl (which outputs `dir` from the cube vertex position). Skybox.ps's
// sun disk / moon / starfield are dropped on purpose: they are time-of-day effects rendered
// every frame, not baked into static probe data.

TextureCube  gSkyCube : register(t6, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define gSampler SmpFixed(SMP_LINEAR_WRAP)

struct PSIn
{
    float4 pos : SV_POSITION;
    float3 dir : TEXCOORD0;  // local cube position = world direction (no translation)
};

float4 main(PSIn i) : SV_TARGET
{
    // Alpha is geometry coverage, not generic opacity. Sky is sampled again
    // directionally at runtime, so keeping it out of the parallax-corrected
    // probe prevents the captured sky from warping against the probe AABB.
    return float4(gSkyCube.SampleLevel(gSampler, normalize(i.dir), 0).rgb, 0.0);
}
