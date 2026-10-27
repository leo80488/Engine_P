// Which surface a half-res texel of the volumetric god-ray tier marches to, and the depth
// key its output carries. Shared by VolumetricRaymarch.cs.hlsl,
// VolumetricRaymarchTemporal.cs.hlsl and VolumetricRaymarchApply.ps.hlsl so the three
// agree on both.
//
// A half-res texel covers a 2x2 block of full-res pixels and marches one of those pixels'
// rays to that pixel's own depth - never the bilinear average of the block's depths,
// which on a silhouette (reversed-Z averages to a harmonic mean of view-Z) is a surface
// that exists nowhere. Checkerboard: even texels take the block's farthest surface, odd
// texels its nearest, so a full-res pixel on either side of an edge finds texels of its
// own surface among the 4 the bilateral apply weighs.
//
// The texel writes the planar view-Z it marched to into .a of its output (its in-scatter
// carries the transmittance from the camera, so none is stored there). The apply compares it
// with each pixel's own key, so a pixel only takes in-scatter marched to a surface like its own.
// Plain bilinear upsampling would draw a half-res staircase along every silhouette in fog
// that TAA / the upscaler cannot resolve: the half-res lattice does not move with the
// projection jitter.
//
// Keep this file pure ASCII: the offline SDK dxc reads the system code page.

#ifndef VOL_RAYMARCH_COMMON_HLSLI
#define VOL_RAYMARCH_COMMON_HLSLI

// The full-res pixel half-res texel `t` marches, and its reversed-Z depth.
int2 VolRmMarchPixel(Texture2D<float> depth, uint2 t, out float ndcZ)
{
    uint fullW, fullH;
    depth.GetDimensions(fullW, fullH);
    const int2 maxP    = int2(fullW, fullH) - 1;
    const int2 base    = int2(t) * 2;
    const bool nearest = ((t.x ^ t.y) & 1u) != 0;
    int2  best  = min(base, maxP);
    float bestZ = depth.Load(int3(best, 0));
    [unroll] for (int i = 1; i < 4; ++i)
    {
        const int2  p = min(base + int2(i & 1, i >> 1), maxP);
        const float z = depth.Load(int3(p, 0));
        // Reversed-Z: larger is nearer, the sky (0) is the farthest.
        if (nearest ? (z > bestZ) : (z < bestZ)) { best = p; bestZ = z; }
    }
    ndcZ = bestZ;
    return best;
}

// The key the march, the temporal pass and the apply compare: the surface's
// planar view-Z, clamped to the far end of the fog range - where every ray's
// march ends anyway; the sky lands there too.
float VolRmKey(float ndcZ, float nearZ, float farZ, float froxelFar)
{
    if (ndcZ <= 1e-6) return froxelFar;
    return min(nearZ * farZ / (nearZ + ndcZ * (farZ - nearZ)), froxelFar);
}

// How much a texel that marched to `texelKey` speaks for a pixel whose own
// surface is at `pixelKey`: 1 for the same surface, ~0 across a silhouette.
// Relative, so the tolerance scales with distance. In-scatter is smooth in
// distance, so the falloff is softer than the cloud composite's (16).
float VolRmKeyWeight(float texelKey, float pixelKey)
{
    const float err = abs(texelKey - pixelKey) / max(min(texelKey, pixelKey), 0.05);
    return exp2(-8.0 * err);
}
#endif // VOL_RAYMARCH_COMMON_HLSLI
