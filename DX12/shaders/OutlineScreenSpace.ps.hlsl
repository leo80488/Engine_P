// Screen-space edge detection of the outline system. Reads:
//   t2 space0 - object ID mask (R32_UINT, written by the ObjectID sub-pass)
//   t3 space0 - GBuffer normal (world normal in [0,1])
//   t5 space0 - GBuffer depth  (hardware depth [0,1])
// Detects edges from three sources and composites the outline color onto HDR:
//   1. object ID discontinuity - different outlined objects adjacent
//   2. depth discontinuity     - Roberts cross on linearized depth (relative)
//   3. normal discontinuity    - Roberts cross on the normal angle
// Distance fade: outlines vanish beyond outlineFadeEnd.

cbuffer OutlineCB : register(b2, space0)
{
    float    outlinePixels;
    float    depthThreshold;
    float    normalThreshold;
    float    outlineStrength;
    float3   outlineColor;
    float    outlineFadeStart;
    uint     outlinePxSpaceW;   // display extents (unused here; keeps the b2 layout)
    uint     outlinePxSpaceH;
    float    outlineFadeEnd;
    float    nearZ;
    float    farZ;
    float3   pickingOutlineColor;   // editor selection color; used when ObjectID bit 31 is set
};

static const uint kPickingMask = 0x80000000u;

Texture2D<uint>   gObjectID : register(t2, space0);
Texture2D<float4> gNormal   : register(t3, space0);
Texture2D<float>  gDepth    : register(t5, space0);

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

// Linearize reversed-Z hardware depth [0,1] to view-space distance:
// d = 1 at near, d = 0 at far -> view.z = n*f / (n + d*(f-n)).
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float LinearizeDepth(float d)
{
    return LinearizeReverseZ(d, nearZ, farZ);
}

float4 main(PSIn i) : SV_TARGET
{
    int2 px = int2(i.pos.xy);

    // ---- Object ID edge ----
    uint id00 = gObjectID.Load(int3(px + int2(0,0), 0));
    uint id10 = gObjectID.Load(int3(px + int2(1,0), 0));
    uint id01 = gObjectID.Load(int3(px + int2(0,1), 0));
    uint id11 = gObjectID.Load(int3(px + int2(1,1), 0));

    // Picking edge: a silhouette transition in the bit-31 mask. Ignores depth, normal
    // and distance fade, so the editor selection shows through walls and never fades
    // with distance (Blender / Unity style).
    bool p00 = (id00 & kPickingMask) != 0;
    bool p10 = (id10 & kPickingMask) != 0;
    bool p01 = (id01 & kPickingMask) != 0;
    bool p11 = (id11 & kPickingMask) != 0;
    bool pickingEdge = (p00 != p10) || (p00 != p01) || (p00 != p11)
                    || (p10 != p01) || (p10 != p11) || (p01 != p11);

    if (pickingEdge)
        return float4(pickingOutlineColor, 1.0);

    // Inside the picking silhouette (any neighbour tagged): this pixel is covered by
    // the picked entity, but the depth buffer belongs to the occluder, so the regular
    // depth / normal edge logic would paint stray dark outlines on the wall. Discard.
    if (p00 || p10 || p01 || p11)
        discard;

    // ---- Regular (material) outline: depth-tested, distance-faded ----
    float d00 = gDepth.Load(int3(px, 0));
    float linearDist = LinearizeDepth(d00);
    if (linearDist > outlineFadeEnd) discard;

    float distFade = 1.0 - saturate(
        (linearDist - outlineFadeStart) / max(outlineFadeEnd - outlineFadeStart, 0.01)
    );

    bool idEdge = false;
    if (id00 != 0 || id10 != 0 || id01 != 0 || id11 != 0)
    {
        idEdge = (id00 != id10) || (id00 != id01) || (id00 != id11)
              || (id10 != id01) || (id10 != id11) || (id01 != id11);
    }

    // ---- Depth edge, linearized and relative ----
    float ld11 = LinearizeDepth(gDepth.Load(int3(px + int2(1,1), 0)));
    float ld10 = LinearizeDepth(gDepth.Load(int3(px + int2(1,0), 0)));
    float ld01 = LinearizeDepth(gDepth.Load(int3(px + int2(0,1), 0)));

    float depthEdge = (abs(linearDist - ld11) + abs(ld10 - ld01)) / max(linearDist, 0.001);
    bool hasDepthEdge = (depthEdge > depthThreshold);

    // ---- Normal edge, clamped distance scaling ----
    float3 n00 = gNormal.Load(int3(px + int2(0,0), 0)).rgb * 2.0f - 1.0f;
    float3 n11 = gNormal.Load(int3(px + int2(1,1), 0)).rgb * 2.0f - 1.0f;
    float3 n10 = gNormal.Load(int3(px + int2(1,0), 0)).rgb * 2.0f - 1.0f;
    float3 n01 = gNormal.Load(int3(px + int2(0,1), 0)).rgb * 2.0f - 1.0f;

    float nDiff0 = 1.0f - saturate(dot(normalize(n00), normalize(n11)));
    float nDiff1 = 1.0f - saturate(dot(normalize(n10), normalize(n01)));

    float distNormalScale = clamp(linearDist / max(outlineFadeStart, 0.1), 0.3, 2.0);
    float scaledNormalThreshold = normalThreshold * distNormalScale;
    bool hasNormalEdge = ((nDiff0 + nDiff1) > scaledNormalThreshold);

    // ---- Composite ----
    bool isOutlinedPixel = (id00 != 0);
    bool isEdge = idEdge || (isOutlinedPixel && (hasDepthEdge || hasNormalEdge));
    if (!isEdge) discard;

    float alpha = outlineStrength * distFade;
    if (alpha < 0.001) discard;
    return float4(outlineColor, alpha);
}
