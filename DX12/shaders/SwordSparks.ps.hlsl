// Niagara sprite appearance. The source curve is stored PER PARTICLE in
// texIdx, so many differently colored emitters safely share one draw bucket.
#define PARTICLE_HAS_SURFACE
#include "particle_surface.hlsli"
ParticleSurface EvaluateParticleSurface(ParticleVaryings i)
{
    ParticleSurface s;
    float4 color = i.color;
    float4 shape = float4(0.5, 2.5, 0, 0);
    if (i.texIdx != 0xFFFFFFFFu)
    {
        float u = (saturate(i.lifeT) * 255 + 0.5) / 256;
        color = g_AllTextures[NonUniformResourceIndex(i.texIdx)].SampleLevel(gLinear, float2(u, 0.25), 0);
        shape = g_AllTextures[NonUniformResourceIndex(i.texIdx)].SampleLevel(gLinear, float2(u, 0.75), 0);
    }
    float r = length(i.uv - 0.5) / max(shape.x, 0.0001);
    // The pack's radial exponential sprite and Niagara's default sphere mask.
    // RadialGradientExponential defaults to ExponentialDensity's Exp2 branch:
    // 1 - exp(-((1-distance/radius)*density)^2), zero outside the radius.
    float densityDepth = max(1-r, 0) * shape.y;
    float alpha = shape.z > 0.5 ? saturate((1-r) * 2)
                               : 1 - exp(-densityDepth * densityDepth);
    s.color = color.rgb;
    s.alpha = color.a * alpha;
    return s;
}
