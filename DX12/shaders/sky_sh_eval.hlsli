#ifndef SKY_SH_EVAL_HLSLI
#define SKY_SH_EVAL_HLSLI

// The sky's L2 spherical-harmonic diffuse with the coefficient buffer passed in. It
// declares no register and opens no heap, so a shader under any root signature can call
// it. The conventions (E(N)/pi, the clamped-cosine constants, the basis SkySHProjection.cs
// projects with) are at the top of sky_sh.hlsli, the graphics binding (gSkySH at t19).
// RTReflectionPass's signature binds the same buffer at t22 (HybridReflIntersectHW.cs,
// HR_EvalSkySH).

float3 EvalSH2From(StructuredBuffer<float4> sh, float3 N)
{
    // A_l / pi - Ramamoorthi & Hanrahan clamped-cosine convolution / pi.
    const float c0 = 1.0;        // A_0/pi = pi/pi
    const float c1 = 2.0 / 3.0;  // A_1/pi = (2pi/3)/pi
    const float c2 = 1.0 / 4.0;  // A_2/pi = (pi/4)/pi

    float b0 = 0.282095;
    float b1 = 0.488603 * N.y;
    float b2 = 0.488603 * N.z;
    float b3 = 0.488603 * N.x;
    float b4 = 1.092548 * N.x * N.y;
    float b5 = 1.092548 * N.y * N.z;
    float b6 = 0.315392 * (3.0 * N.z * N.z - 1.0);
    float b7 = 1.092548 * N.z * N.x;
    float b8 = 0.546274 * (N.x * N.x - N.y * N.y);

    return max(0.0,
        sh[0].rgb * (b0 * c0) +
        sh[1].rgb * (b1 * c1) +
        sh[2].rgb * (b2 * c1) +
        sh[3].rgb * (b3 * c1) +
        sh[4].rgb * (b4 * c2) +
        sh[5].rgb * (b5 * c2) +
        sh[6].rgb * (b6 * c2) +
        sh[7].rgb * (b7 * c2) +
        sh[8].rgb * (b8 * c2));
}

// Band 0 alone: the spherical average of E(N)/pi over all normals (why a
// stylized receiver wants it: sky_sh.hlsli's EvalSH2Isotropic).
float3 EvalSH2IsotropicFrom(StructuredBuffer<float4> sh)
{
    return max(0.0, sh[0].rgb * 0.282095);
}

#endif // SKY_SH_EVAL_HLSLI
