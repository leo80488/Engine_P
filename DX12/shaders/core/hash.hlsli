// core/hash.hlsli - integer and float hashes, and the low-discrepancy orderings
// built on them. The rules every core header follows are at the top of
// core/gbuffer_codec.hlsli.
//
// One rule of its own: never alter a hash here or merge two of them. A different
// hash is a different noise pattern - ReSTIR's reservoirs, TAA and shadow
// dithering, grass placement, particle flicker - not a refactor. The variants kept
// in their passes (other output mappings, IGN, R2 flavours) stay there for that
// reason.

#ifndef CORE_HASH_HLSLI
#define CORE_HASH_HLSLI

// lowbias32 (C. Wellons, hash-prospector).
uint HashLowbias32(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

// PCG hash (Jarzynski & Olano 2020, "Hash Functions for GPU Rendering").
uint HashPcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Wang hash (T. Wang, 32-bit integer version).
uint HashWang(uint s)
{
    s = (s ^ 61u) ^ (s >> 16u);
    s *= 9u;
    s ^= s >> 4u;
    s *= 0x27d4eb2du;
    s ^= s >> 15u;
    return s;
}

// The classic sin hash, 2D -> [0,1)^2. Its values depend on the GPU's sin
// precision; terrain detail and grass placement are built on them, so do not
// replace it.
float2 HashSin22(float2 p)
{
    p = float2(dot(p, float2(127.1, 311.7)),
               dot(p, float2(269.5, 183.3)));
    return frac(sin(p) * 43758.5453123);
}

// D. Hoskins' "hash without sine" 2D -> [0,1) (cheap value noise for beams,
// tracers, particle flicker and trails).
float HashHoskins12(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

// A 3D cell hash that tiles: the integer cell wraps mod gridDim, so noise baked
// from it repeats exactly across the volume texture (the cloud noise bakes).
float3 HashCellTiled3(int3 c, int3 gridDim)
{
    c = ((c % gridDim) + gridDim) % gridDim;
    float3 p = float3(c) + 0.5;
    p = frac(p * float3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return frac(float3((p.x + p.y) * p.z,
                       (p.y + p.z) * p.x,
                       (p.z + p.x) * p.y));
}

// A stable per-pixel Cranley-Patterson base (PCG of the pixel), advanced per
// frame by the R2 sequence (plastic constant): the AMD SSSR / Lumen pattern
// the ray-traced reflections, AO and sun shadows sample with.
float2 R2PixelFrame2D(uint2 p, uint frame)
{
    const uint w = HashPcg(p.x * 0x9E3779B9u ^ p.y * 0x85EBCA77u);
    float2 base = float2(w & 0xFFFFu, w >> 16u) * (1.0 / 65536.0);
    const float2 kR2 = float2(0.7548776662, 0.5698402910);
    return frac(base + kR2 * float(frame & 0xFFFu));
}

// The Hilbert-curve index of a pixel inside its 64x64 tile (XeGTAO's
// ordering): neighbouring pixels get distant indices, which keeps an R2
// sequence indexed by it blue-noise-like.
uint HilbertIndex64(uint2 p)
{
    p &= 63u;
    uint i = 0u;
    for (uint level = 32u; level > 0u; level >>= 1u)
    {
        const uint rx = (p.x & level) > 0u ? 1u : 0u;
        const uint ry = (p.y & level) > 0u ? 1u : 0u;
        i += level * level * ((3u * rx) ^ ry);
        if (ry == 0u)
        {
            if (rx == 1u) p = 63u - p;
            p = p.yx;
        }
    }
    return i;
}

#endif // CORE_HASH_HLSLI
