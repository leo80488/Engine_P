// core/packing.hlsli - vectors packed into fewer channels. The rules every core header
// follows are at the top of core/gbuffer_codec.hlsli.

#ifndef CORE_PACKING_HLSLI
#define CORE_PACKING_HLSLI

// ==== Octahedral unit vector <-> [-1,1]^2 ====
// The sphere folded onto the octahedron |x|+|y|+|z| = 1 and the lower half unfolded over the
// square's corners. A zero sign counts as positive, and the 1e-6 guard keeps a degenerate
// (zero) input finite. Callers quantize e*0.5+0.5 themselves (10 bits in the ReSTIR / SSGI
// reservoir meta, 16 in RTXDI's surface record). Not the GBuffer normal, which is three UNORM
// channels.
// OctSignNotZero: +1 or -1 per component, zero counting as positive. Scalar ternaries: a
// vector condition is an error under HLSL 2021 (RtxdiPT compiles with -HV 2021).
float2 OctSignNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float2 OctEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    float2 e = n.xy;
    if (n.z < 0.0)
        e = (1.0 - abs(e.yx)) * OctSignNotZero(e);
    return e;
}

float3 OctDecode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * OctSignNotZero(n.xy);
    return normalize(n);
}

#endif // CORE_PACKING_HLSLI
