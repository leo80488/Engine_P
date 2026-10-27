#ifndef SKY_SH_HLSLI
#define SKY_SH_HLSLI

// L2 spherical-harmonic sky diffuse, projected by SkyIBLPass from whichever cubemap
// drives the sky (procedural atmosphere: every frame; static .itex: once) into 9 x float4
// coefficients. SkySHProjection.cs stores the raw radiance projection
// L_lm = integral of L(w) Y_lm(w) dw; EvalSH2(N) convolves with the clamped-cosine kernel
// (Ramamoorthi & Hanrahan 2001), so it returns diffuse irradiance / pi, usable directly
// as `albedo * EvalSH2(N)` in a Lambertian shader.
//
// Convolution constants A_0 = pi, A_1 = 2pi/3, A_2 = pi/4, folded with 1/pi:
// c0 = 1, c1 = 2/3, c2 = 1/4.
//
// Why it matters under a Rayleigh sky: the dome is strongly blue at zenith and warmer at
// the horizon. Unconvolved reconstruction returns the band-limited L(N), so an upward
// normal gets the peak blue and a white object tints blue. The convolution suppresses the
// L=2 band (the zenith-vs-horizon non-uniformity) by 4x and yields the physical
// irradiance / pi.
//
// Magnitude convention shared by every diffuse source: E(N)/pi - this file, DDGI
// (DDGISampling.hlsli's DDGI_SampleProbeIrradiance) and gIrradiance (the prefiltered
// irradiance cube).
//
// Used by the deferred LightingPass and the forward TransparentPass, same buffer, same
// evaluator; LightCB.iblUseSH gates the call so static-skybox scenes still sample
// gIrradiance. Slot (must match the LightingPass / TransparentPass root signature):
// t19 space0, gSkySH StructuredBuffer<float4>. The basis matches SkySHProjection.cs.hlsl;
// keep the constants in sync.

#include "sky_sh_eval.hlsli"   // EvalSH2From / EvalSH2IsotropicFrom: the math, buffer passed in

// The graphics binding: the evaluator on t19.
StructuredBuffer<float4> gSkySH : register(t19, space0);

float3 EvalSH2(float3 N)
{
    return EvalSH2From(gSkySH, N);
}

// Band-0 (DC) term of EvalSH2 alone: the spherical average of E(N)/pi over all normals.
// Direction-free, so it carries the sky's average hue, not the zenith (a fixed up-vector
// tap under a Rayleigh sky returns the blue dome peak). For stylized receivers whose form
// model is the toon ramp: ambient at the vertex normal re-adds a second form gradient
// whose C1 creases show at triangle edges; this removes the normal from the ambient
// response entirely.
float3 EvalSH2Isotropic()
{
    return EvalSH2IsotropicFrom(gSkySH);
}

#endif // SKY_SH_HLSLI
