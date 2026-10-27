#ifndef SHADING_MODELS_HLSLI
#define SHADING_MODELS_HLSLI

// GBuffer codec for the per-pixel data the non-Standard shading models need, shared by
// the writer and the reader. The encode lives in the GBuffer pixel template
// (material_surface.hlsli's MS_EncodeGBuffer, which every material, engine default and
// custom hook alike, goes through), the decode in Lighting.ps.hlsl. A packing that drifts
// between the two still compiles and gives a wrong highlight direction that looks like an
// art bug, hence one header.
//
// No resource access in this file: like lighting_types.hlsli / light_eval.hlsli it is
// shared by passes under different root signatures, each owning its resources. Pure
// arithmetic only.
//
// RT4 ("extra", R16G16B16A16_FLOAT) channel budget - the only place these models can
// live, and already contended:
//
//   channel | Standard / NPR      | Anisotropic        | Subsurface
//   --------+---------------------+--------------------+--------------------
//    .r     | face SDF right half | aniso angle [0,1)  | SSS opacity  [0,1]
//    .g     | face hair mark      | anisotropy [-1,1]  | SSS mask     [0,1]
//    .b     | hair mask (0/1)     | hair mask (0/1)    | hair mask (0/1)
//    .a     | face marker (>=1)   | same               | same
//           | or directAdditive   |                    |
//           | (<0.5, see          |                    |
//           |  material_types)    |                    |
//
//   .b and .a are model-independent and this file never touches them, so an anisotropic
//   hair material still gets its FaceOverlay mask and its stylized direct-additive. .r/.g
//   are model-owned, which is why MS_EncodeGBuffer never stamps them on a face pixel
//   (marker in .a): a V2 face uses all four channels and there is no fifth.

#include "core/math.hlsli"   // PI

// ==== Reference tangent frame from a normal alone ====
// The frame is core/sampling.hlsli's OrthonormalBasis (Duff et al. 2017): branchless and
// stable for every unit N, N.z == -1 included. It is discontinuous across the N.z == 0
// great circle, where `s` flips and the two basis vectors swap orientation. That is
// harmless here only because encode and decode run on the same N and the deferred pass
// reads RT4 with a texel-exact 1:1 fetch (LightingPass's linear-clamp GBuffer sampler on
// a full-resolution target equals point sampling at texel centers, as the exact 16-bit
// matIdx decode from RT1.a relies on too). Do not reuse this codec on a filtered
// channel: a bilinear tap across the seam interpolates two unrelated angles and yields a
// direction that belongs to neither pixel.
#include "core/sampling.hlsli"   // OrthonormalBasis, SampleGGXVNDF, Hammersley2D

// ==== The normal the decoder will see ====
// Makes encoder and decoder branch on the same value across the reference frame's seam;
// do not "simplify" the encoder back to its own normalize(s.normalWS).
//
// OrthonormalBasis branches on `N.z >= 0`, and the encoder holds a float32 normal while
// the decoder reads it back through R16G16B16A16_UNORM. If they disagree about that sign
// they build mirrored bases and the decoded azimuth is wrong along the whole N.z == 0
// great circle - a contour, not an isolated pixel.
//
// The UNORM midpoint lands exactly on N.z == 0, so quantisation alone keeps the sign; the
// only disagreement comes from float32: for |N.z| below ~1e-8, `n.z*0.5 + 0.5` rounds to
// exactly 0.5 (eps at 0.5 is 2^-25), so a tiny negative n.z reaches the ROP as positive.
// That band is far below one pixel, but the guard costs ~6 ALU on anisotropic pixels only
// and makes the codec safe by construction, also if RT1 changes format or packing.
//
// Pass it `o.normal.rgb`, the exact float the ROP is about to quantise.
float3 SM_DequantizeGBufferNormal(float3 packed01)
{
    const float3 q = floor(saturate(packed01) * 65535.0 + 0.5);
    return normalize(q * (1.0 / 65535.0) * 2.0 - 1.0);
}

// ==== Anisotropy direction <-> one scalar channel ====
// Stored as the angle of the direction within N's tangent plane, measured against
// OrthonormalBasis: one channel instead of two, and the decoded direction is unit-length
// by construction (no quantised vector to renormalise, no degenerate zero).
//
// Half-turn range, deliberately: an anisotropic GGX lobe is symmetric under T -> -T (D
// and the Smith visibility see the tangent only through dot(T, .) in a squared term), so
// the discarded 180 degrees carry no information. [0,1) maps to [0,pi): ~0.09 deg of
// quantisation at FP16's 11-bit mantissa, three orders below anything visible.
float SM_EncodeAnisoDir(float3 N, float3 dirWS)
{
    float3 refT, refB;
    OrthonormalBasis(N, refT, refB);

    // Project into the tangent plane. A direction parallel to N carries no anisotropy (the
    // projection vanishes): return the reference tangent rather than an undefined atan2(0,0).
    const float2 t = float2(dot(dirWS, refT), dot(dirWS, refB));
    if (dot(t, t) < 1e-12) return 0.0;

    // atan2 -> [-pi, pi]; dividing by pi gives [-1, 1] and frac() folds that to [0, 1),
    // the mod-pi fold the T -> -T symmetry allows (frac(-0.3) == 0.7).
    return frac(atan2(t.y, t.x) * (1.0 / PI));
}

// Inverse of SM_EncodeAnisoDir. Returns a unit vector in N's tangent plane.
float3 SM_DecodeAnisoDir(float3 N, float angle01)
{
    float3 refT, refB;
    OrthonormalBasis(N, refT, refB);
    float s, c;
    sincos(saturate(angle01) * PI, s, c);
    return refT * c + refB * s;
}

// ==== Anisotropic tangent frame for the lighting pass ====
// Decodes RT4.r into the (T, B) pair the anisotropic BRDF wants. B = cross(N, T) is
// already unit-length: SM_DecodeAnisoDir makes T unit and perpendicular to N.
void SM_AnisoFrame(float3 N, float angle01, out float3 T, out float3 B)
{
    T = SM_DecodeAnisoDir(N, angle01);
    B = cross(N, T);
}

// ==== Anisotropic bent normal (IBL / reflection-probe lookups) ====
// Filament's construction. The split-sum environment lookup is built on one reflection
// vector and one isotropic roughness, so the honest way to give an anisotropic surface an
// anisotropic environment reflection without re-baking the BRDF LUT is to bend the normal
// the reflection is taken about, which stretches the reflected image along the right axis.
//
// Returns N exactly when anisotropy == 0, so isotropic pixels stay bit-identical.
float3 SM_AnisotropicBentNormal(float3 N, float3 V, float3 T, float3 B,
                                float anisotropy, float roughness)
{
    if (anisotropy == 0.0) return N;

    const float3 anisoDir     = (anisotropy >= 0.0) ? B : T;
    const float3 anisoTangent = cross(anisoDir, V);
    const float3 anisoNormal  = cross(anisoTangent, anisoDir);
    // Rough surfaces bend more: at mirror roughness the lobe is a point and
    // bending it just aims the reflection somewhere wrong.
    const float  bend         = abs(anisotropy) * saturate(5.0 * roughness);
    const float3 bent         = lerp(N, anisoNormal, bend);
    const float  len2         = dot(bent, bent);
    return (len2 > 1e-8) ? bent * rsqrt(len2) : N;
}

#endif // SHADING_MODELS_HLSLI
