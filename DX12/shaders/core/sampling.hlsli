// Frames and sample sequences. The rules every core header follows are at the top of
// core/gbuffer_codec.hlsli.

#ifndef CORE_SAMPLING_HLSLI
#define CORE_SAMPLING_HLSLI

// ==== Frames from a normal ====
// Duff et al. 2017, "Building an Orthonormal Basis, Revisited": branchless and
// numerically stable for every unit n, n.z == -1 included. It is discontinuous across
// n.z == 0, where s flips and t / b swap orientation: harmless where the frame is rebuilt
// from the same n (ray sampling, the RT4 anisotropy codec), not across a filtered channel
// (shading_models.hlsli explains).
void OrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    const float s = (n.z >= 0.0) ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// A tangent frame for geometry without authored tangents: cross with +Y, or +Z
// when the normal is within ~8 degrees of +-Y (cross with a parallel axis would
// be zero and normalize(0) a NaN that poisons the whole triangle).
void TangentFrameRefY(float3 n, out float3 t, out float3 b)
{
    float3 ref = (abs(n.y) > 0.99) ? float3(0, 0, 1) : float3(0, 1, 0);
    t = normalize(cross(ref, n));
    b = cross(n, t);
}

// The same with +Z as the reference axis (+X near +-Z): the split-sum prefilter
// and SSR's GGX sampling frame. Not interchangeable with TangentFrameRefY - the
// frames differ by a rotation about n, which moves every sample.
void TangentFrameRefZ(float3 n, out float3 t, out float3 b)
{
    float3 up = abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

// ==== GGX ====
// Heitz 2018 visible-NDF sample. Ve = the view direction in tangent space (+Z = N);
// returns the sampled half vector in the same space.
float3 SampleGGXVNDF(float3 Ve, float alpha, float u1, float u2)
{
    const float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    const float  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    const float3 T1 = select(lensq > 1e-7, float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq),
                             float3(1.0, 0.0, 0.0));
    const float3 T2 = cross(Vh, T1);
    const float  r   = sqrt(u1);
    const float  phi = 6.28318530718 * u2;
    const float  t1  = r * cos(phi);
    float        t2  = r * sin(phi);
    const float  s   = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(saturate(1.0 - t1 * t1)) + s * t2;
    const float3 Nh = t1 * T1 + t2 * T2
                    + sqrt(saturate(1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(1e-6, Nh.z)));
}

// ==== Low-discrepancy points ====
// Base-2 radical inverse (Van der Corput): the bits of i mirrored about the point.
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;   // / 2^32
}

// Point i of the n-point 2D Hammersley set.
float2 Hammersley2D(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdC(i));
}

#endif // CORE_SAMPLING_HLSLI
