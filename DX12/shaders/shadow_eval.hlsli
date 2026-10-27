#ifndef SHADOW_EVAL_HLSLI
#define SHADOW_EVAL_HLSLI

// Directional-shadow math with every resource and constant passed in: the per-pixel PCF
// rotation, the receiver-plane depth bias and the 16-tap Vogel PCF that the cascades, the
// per-object map and the hair map all run. No register, no heap. The binding is
// shadow.hlsli (the graphics signature's LightCB, gShadowCascades (t9), gShadowSampler (s2)
// and the heap-opened maps). Cascade selection stays there too: shadowMatrix[idx] is
// dynamically indexed, and copying that CB array into a parameter would make an
// indexable temp.

static const float kBayer4x4[16] = {
     0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
    12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
     3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
    15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0,
};
// Per-frame PCF rotation. The (frameIndex & 3) / 4 turn adds 4/16 to every Bayer value,
// one fixed rearrangement of the 4x4 tile: in every penumbra the dither travels one row
// down per frame on a 4-frame cycle (noise that scrolls in Y on shadowed walls). With
// temporal AA / DLSS (the CPU sets bit 31 of the frame index) the tile is re-seated at a
// hashed offset with a golden-ratio phase each frame, so what the temporal filter resolves
// has no coherent travel. Without it the fixed rotation stays: the zero-tolerance golden
// restir_off_preintegration pins that path.
#include "core/hash.hlsli"
float ShadowRotationAngleFrom(float2 screenPos, uint frameIndex)
{
    if ((frameIndex & 0x80000000u) == 0u)
    {
        const uint2 px          = uint2(screenPos) & 3u;
        const float frameOffset = (float)(frameIndex & 3u) * (1.0 / 4.0);
        return (kBayer4x4[px.x + px.y * 4u] + frameOffset) * 6.28318530718;
    }
    const uint  index = frameIndex & 0x7FFFFFFFu;
    const uint  h     = HashLowbias32(index);
    const uint2 px    = (uint2(screenPos) + uint2(h, h >> 2)) & 3u;
    const float phase = frac(float(index & 0xFFFFu) * 0.61803398875);
    return frac(kBayer4x4[px.x + px.y * 4u] + phase) * 6.28318530718;
}

// Receiver-plane depth gradient: how does shadow-space z change per
// shadow-space UV step on the receiving surface? Solving
//   [du/dx du/dy] [dz/du]   [dz/dx]
//   [dv/dx dv/dy] [dz/dv] = [dz/dy]
// via Cramer's rule. Magnitude is clamped - past ~1.0 in (z per uv) we are
// usually on a quad that straddles a depth or cascade discontinuity, where the
// linear-plane assumption breaks and the gradient becomes a noise source
// instead of a bias source.
float2 ReceiverPlaneDepthBias(float3 shadowUVZ)
{
    float3 dx = ddx(shadowUVZ);
    float3 dy = ddy(shadowUVZ);

    // Cascade-boundary guard. Across a 2x2 quad that straddles a cascade split,
    // adjacent pixels select DIFFERENT cascades, so the shadow-space UV jumps by
    // a large fraction of the map between neighbours (vs ~1 texel within a
    // cascade). The screen-space ddx/ddy then explode and this receiver-plane
    // bias goes garbage -> a thin black acne line exactly on the cascade boundary
    // (visible at grazing camera + low sun). A real per-pixel step is ~1/mapSize
    // (<=~0.0005); anything >> that is cross-cascade divergence -> drop RPDB and
    // let normal-offset + the constant receiver bias cover this pixel.
    if (max(max(abs(dx.x), abs(dx.y)), max(abs(dy.x), abs(dy.y))) > 0.05)
        return float2(0.0, 0.0);

    float det = dx.x * dy.y - dx.y * dy.x;
    float2 grad;
    grad.x = dy.y * dx.z - dx.y * dy.z;
    grad.y = dx.x * dy.z - dy.x * dx.z;
    float safeDet = (abs(det) > 1e-8) ? det : (det >= 0 ? 1e-8 : -1e-8);
    grad /= safeDet;
    return clamp(grad, float2(-1.0, -1.0), float2(1.0, 1.0));
}

// Vogel-disk 16-tap PCF around uv (radius in texels times texelSize), rotated by
// baseAngle. Each tap compares against the depth the receiving plane predicts
// there (depthCenter + rpdb . offset) plus `bias`; reversed-Z, so a positive
// bias raises the reference and biases toward lit. The cascade array's version:
float VogelPcf16(Texture2DArray<float> map, SamplerComparisonState cmp, float2 uv, float slice,
                 float depthCenter, float2 rpdb, float bias, float radius, float texelSize,
                 float baseAngle)
{
    const int   kTaps   = 16;
    const float kGolden = 2.39996323;  // golden angle

    float shadow = 0.0;
    [unroll]
    for (int i = 0; i < kTaps; ++i)
    {
        float  r     = sqrt(((float)i + 0.5) / (float)kTaps);
        float  theta = (float)i * kGolden + baseAngle;
        float2 off   = float2(cos(theta), sin(theta)) * r * radius * texelSize;
        float  depthAtTap = depthCenter + dot(rpdb, off) + bias;
        shadow += map.SampleCmpLevelZero(cmp, float3(uv + off, slice), depthAtTap);
    }
    return shadow * (1.0 / (float)kTaps);
}

// ... and a single map's (the per-object and hair shadows).
float VogelPcf16(Texture2D<float> map, SamplerComparisonState cmp, float2 uv,
                 float depthCenter, float2 rpdb, float bias, float radius, float texelSize,
                 float baseAngle)
{
    const int   kTaps   = 16;
    const float kGolden = 2.39996323;

    float shadow = 0.0;
    [unroll]
    for (int i = 0; i < kTaps; ++i)
    {
        float  r     = sqrt(((float)i + 0.5) / (float)kTaps);
        float  theta = (float)i * kGolden + baseAngle;
        float2 off   = float2(cos(theta), sin(theta)) * r * radius * texelSize;
        float  depthAtTap = depthCenter + dot(rpdb, off) + bias;
        shadow += map.SampleCmpLevelZero(cmp, uv + off, depthAtTap);
    }
    return shadow * (1.0 / (float)kTaps);
}

#endif // SHADOW_EVAL_HLSLI
