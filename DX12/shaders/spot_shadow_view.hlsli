#ifndef SPOT_SHADOW_VIEW_HLSLI
#define SPOT_SHADOW_VIEW_HLSLI
struct SpotShadowView
{
    float4x4 vp;
    uint atlasIndex;
    uint slice;
    uint resolution;
    uint padding;
};
#endif
