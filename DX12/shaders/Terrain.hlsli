#ifndef TERRAIN_HLSLI
#define TERRAIN_HLSLI

// Cubic B-spline filtered heightmap sample from 4 bilinear taps.
// Bilinear is C0 but not C1: the slope jumps at every texel boundary, so a low-precision heightmap
// (BC1/BC4 ~8 levels per 4x4 block, 8-bit = 256 levels) under a large heightScale shows
// contour-line terraces. The C2 B-spline hides the steps for little extra cost (Sigg & Hadwiger
// 2005; GPU Pro 2 ch. 5). uv is the heightmap UV, uvTexelSize is 1 / heightmap resolution.
float TerrainSampleHeightBicubic(Texture2D<float> tex, SamplerState samp,
                                 float2 uv, float uvTexelSize)
{
    float  texSize  = 1.0 / max(uvTexelSize, 1e-6);
    float2 sample   = uv * texSize;
    float2 tex1     = floor(sample - 0.5) + 0.5;
    float2 f        = sample - tex1;

    // Cubic B-spline weights (Mitchell-Netravali B=1, C=0)
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0/6.0) * (-f3 + 3.0 * f2 - 3.0 * f + 1.0);
    float2 w1 = (1.0/6.0) * (3.0 * f3 - 6.0 * f2 + 4.0);
    float2 w2 = (1.0/6.0) * (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0);
    float2 w3 = (1.0/6.0) * f3;

    float2 g0 = w0 + w1;
    float2 g1 = w2 + w3;
    // Bilinear tap offsets (Sigg & Hadwiger 2005, section 3.2):
    //   h0 = w1/g0 - 1  (texels at offset -1, 0)
    //   h1 = w3/g1 + 1  (texels at offset +1, +2)
    // Do not use w2 in h1: the second tap lands on the wrong texel and shows periodic wave bands at
    // texel frequency.
    float2 h0 = (w1 / g0) - 1.0;
    float2 h1 = (w3 / g1) + 1.0;

    float2 uv0 = (tex1 + h0) / texSize;
    float2 uv1 = (tex1 + h1) / texSize;

    float a = tex.SampleLevel(samp, float2(uv0.x, uv0.y), 0);
    float b = tex.SampleLevel(samp, float2(uv1.x, uv0.y), 0);
    float c = tex.SampleLevel(samp, float2(uv0.x, uv1.y), 0);
    float d = tex.SampleLevel(samp, float2(uv1.x, uv1.y), 0);

    return lerp(lerp(a, b, g1.x), lerp(c, d, g1.x), g1.y);
}

// ==== Hex-grid stochastic tiling ====
// Heitz & Neyret 2018, "By-Example Procedural Stochastic Texturing", reduced to a 3-tap blend without
// histogram preservation. Each hex cell rotates and offsets the texture randomly and the 3 nearest
// cells blend by barycentric weight, which breaks the row/column repetition of layers tiled at
// world-meter scale. Costs 3x the samples: use it on albedo and normal, keep low-frequency maps (ARM,
// displacement) at 1 tap. A cell is 1 unit of input UV: pick layer.tilingScale so a cell is
// ~0.5-1x the texture's repeat distance.

#include "core/hash.hlsli"

// Returns three tap UVs, their weights and each tap's forward rotation as (cos, sin). Normal-map
// callers must back-rotate each sampled tangent-space normal by it before blending (the taps sit in
// different rotated tangent frames); albedo / scalar callers ignore it.
void TerrainStochasticTaps(float2 uv,
                           out float2 outUV0,  out float2 outUV1,  out float2 outUV2,
                           out float  outW0,   out float  outW1,   out float  outW2,
                           out float2 outRot0, out float2 outRot1, out float2 outRot2)
{
    // Skew uv onto a triangular grid: floor() picks the cell vertex, frac() the barycentrics inside the
    // half-cell triangle. The constants invert the 60-deg hexagon basis (sqrt(3)/2 = 0.86602540378).
    const float2 t  = float2(uv.x - 0.5773502692 * uv.y,
                             uv.y * 1.1547005384);
    const float2 ti = floor(t);
    const float2 tf = t - ti;

    // Two triangles tile the cell (lower-left and upper-right). Barycentric
    // weights sum to 1; a vertex on the diagonal seam gets weight from
    // both triangles, so the blend is C0 across the seam.
    float2 v0, v1, v2;
    float3 bary;
    if (tf.x + tf.y < 1.0)
    {
        v0   = ti;
        v1   = ti + float2(1, 0);
        v2   = ti + float2(0, 1);
        bary = float3(1.0 - tf.x - tf.y, tf.x, tf.y);
    }
    else
    {
        v0   = ti + float2(1, 1);
        v1   = ti + float2(1, 0);
        v2   = ti + float2(0, 1);
        bary = float3(tf.x + tf.y - 1.0, 1.0 - tf.y, 1.0 - tf.x);
    }

    // Per-vertex hash -> independent (rotation, offset) per cell.
    float2 h0 = HashSin22(v0);
    float2 h1 = HashSin22(v1);
    float2 h2 = HashSin22(v2);

    // Rotate uv about its origin by a hashed angle, then add a hashed offset. The rotation is what
    // breaks the repetition; an offset alone leaves a visible shift pattern.
    float a0 = h0.x * 6.2831853;     // 2 pi
    float a1 = h1.x * 6.2831853;
    float a2 = h2.x * 6.2831853;
    outRot0 = float2(cos(a0), sin(a0));
    outRot1 = float2(cos(a1), sin(a1));
    outRot2 = float2(cos(a2), sin(a2));

    outUV0 = float2(outRot0.x * uv.x - outRot0.y * uv.y,
                    outRot0.y * uv.x + outRot0.x * uv.y) + h0;
    outUV1 = float2(outRot1.x * uv.x - outRot1.y * uv.y,
                    outRot1.y * uv.x + outRot1.x * uv.y) + h1;
    outUV2 = float2(outRot2.x * uv.x - outRot2.y * uv.y,
                    outRot2.y * uv.x + outRot2.x * uv.y) + h2;

    // Variance-preserving weights: squared barycentrics, renormalised so the blend keeps a single tap's
    // variance (Heitz-Neyret eq. 8, simplified). A plain 3-way lerp over-blurs the transition zones.
    float3 w2     = bary * bary;
    float  invSum = rcp(w2.x + w2.y + w2.z);
    outW0 = w2.x * invSum;
    outW1 = w2.y * invSum;
    outW2 = w2.z * invSum;
}

// Rotates the screen-space UV gradient g by a tap's forward rotation rot = (cos, sin), for SampleGrad.
// Required: a tap UV is R*uv + h with R and h hashed per cell, so ddx(tapUV) across a cell seam mixes
// two unrelated rotations and implicit Sample() picks the coarsest mip; the bogus derivative also
// grows with |uv|, since the rotation is about uv's origin. The true gradient at fixed R, h is
// R*grad(uv). TerrainStochasticTaps applies no scale, so none is applied here.
float2 TerrainRotateGrad(float2 rot, float2 g)
{
    return float2(rot.x * g.x - rot.y * g.y,
                  rot.y * g.x + rot.x * g.y);
}

#endif // TERRAIN_HLSLI
