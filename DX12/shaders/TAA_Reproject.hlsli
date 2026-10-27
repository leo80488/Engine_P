#ifndef TAA_REPROJECT_HLSLI
#define TAA_REPROJECT_HLSLI

#include "TAA_Common.hlsli"

// TAA history reprojection helpers: velocity dilation and the off-screen test.

// Depth-aware velocity dilation (Karis 2014, "High Quality Temporal
// Supersampling"): take the velocity of the 3x3 neighbour nearest the camera. At a
// silhouette between a slow foreground and a fast background (or vice versa),
// longest-magnitude dilation picks the background velocity for foreground-edge
// pixels and the edge ghost-trails the background's motion; nearest depth favours
// the foreground, the surface that dominates the silhouette pixel's shading.

float2 DilateVelocity(Texture2D<float2> velTex,
                       Texture2D<float>  depthTex,
                       int2              pos,
                       int2              dim)
{
    int2  closestPos   = pos;
    float closestDepth = depthTex.Load(int3(pos, 0)).r;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    [unroll]
    for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0) continue;
        int2  p = clamp(pos + int2(dx, dy), int2(0, 0), dim - 1);
        float d = depthTex.Load(int3(p, 0)).r;
        // Reversed-Z (near = 1, far = 0, GREATER_EQUAL test): nearer the camera means a
        // larger depth value.
        if (d > closestDepth)
        {
            closestDepth = d;
            closestPos   = p;
        }
    }
    return velTex.Load(int3(closestPos, 0));
}


// Apply NDC-space velocity to current screen UV.
//   velocity is (curNDC - prevNDC), so prevNDC = curNDC - velocity.
float2 ReprojectUV(float2 uv, float2 velocity)
{
    float2 curNDC  = UVToNDC(uv);
    float2 prevNDC = curNDC - velocity;
    return NDCToUV(prevNDC);
}

bool IsUVOutside(float2 uv)
{
    return any(uv < 0.0) || any(uv > 1.0);
}

#endif // TAA_REPROJECT_HLSLI
