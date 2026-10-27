// EditorGrid.ps.hlsl - editor-only infinite reference grid on the world XZ plane.
//
// Fullscreen triangle drawn after Water and BEFORE CloudPass, so clouds and
// height fog composite over it and transparents / particles draw on top. The
// grid is analytic: each pixel's view ray is intersected with the y == GridY
// plane (no geometry, truly infinite), lines are anti-aliased with screen-space
// derivatives, and occlusion is a manual test against the scene depth. The grid
// never writes depth, so picking, TAA and the post chain are unaffected.
//
// Two line levels: minor cells every CellSize metres (the editor unit selector)
// and major lines every MajorEvery cells; the world X and Z axes get their own
// colors. Each level fades out on its own once its cells drop below a few
// pixels, which also kills horizon moire without a hard cutoff; a radial fade
// caps the far extent.
//
// Blend ONE / INV_SRC_ALPHA (premultiplied-alpha output), like the other
// fullscreen composites (HeightFogApply / VolumetricApply).
//
// Graphics root signature only: LightCB at b1 (cameraPos, invViewProj),
// EditorGridCB at b2, depth at t5 space0 - the same slots as HeightFogApply.

#define LIGHT_CB_REGISTER b1
#include "light_cb.hlsli"      // cameraPos, invViewProj
#include "DepthCommon.hlsli"   // WorldPosFromDepth, ViewRayFromUV

cbuffer EditorGridCB : register(b2, space0)
{
    float  CellSize;       // metres per minor cell (editor unit selector)
    float  MajorEvery;     // minor cells per major line
    float  GridY;          // plane height, world metres
    float  FadeDistance;   // radial fade end, metres

    float4 MinorColor;     // rgb = linear color, a = peak opacity
    float4 MajorColor;
    float4 AxisXColor;     // line along the world X axis (where z == 0)
    float4 AxisZColor;     // line along the world Z axis (where x == 0)
};

Texture2D<float> gDepth : register(t5, space0);

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSIn i) : SV_TARGET
{
    // The ray/plane setup is unconditional so the fwidth() derivatives below stay
    // valid across every 2x2 quad (no divergent early-out before them).
    // The ray ORIGIN is the per-pixel near-plane point, not cameraPos: under an
    // orthographic projection every pixel has its own parallel ray, and a shared
    // cameraPos origin would collapse the grid to one point (for perspective the
    // near-plane point lies on the same ray). Reversed-Z: the near plane is NDC z = 1.
    float3 rayOrigin = WorldPosFromDepth(i.uv, 1.0, invViewProj);
    float3 rayDir    = ViewRayFromUV(i.uv, invViewProj);
    float  dirY   = (abs(rayDir.y) < 1e-8) ? (rayDir.y >= 0.0 ? 1e-8 : -1e-8) : rayDir.y;
    float  tGrid  = (GridY - rayOrigin.y) / dirY;
    float3 wp     = rayOrigin + rayDir * tGrid;

    float2 cell  = wp.xz / CellSize;
    float2 dCell = max(fwidth(cell), 1e-8);

    // Occlusion: reversed-Z, sky is EXACTLY 0 (never an epsilon). The relative
    // bias hides the grid where real geometry is coplanar with it (floors at
    // y == GridY) instead of z-fighting with it.
    float ndcZ   = gDepth.Load(int3(int2(i.pos.xy), 0));
    float tScene = 1e30;
    if (ndcZ > 0.0)
    {
        float3 scenePos = WorldPosFromDepth(i.uv, ndcZ, invViewProj);
        tScene = dot(scenePos - rayOrigin, rayDir);
    }
    float visible = (tGrid > 0.0 && tGrid < tScene - max(tScene * 1e-3, 0.01)) ? 1.0 : 0.0;

    // Minor grid: pixel distance to the nearest cell line, per axis.
    float2 pxMinor   = abs(frac(cell + 0.5) - 0.5) / dCell;
    float  minorLine = 1.0 - saturate(min(pxMinor.x, pxMinor.y));
    float  minorFade = saturate(2.0 - 8.0 * max(dCell.x, dCell.y)); // gone once cells < ~4 px

    // Major grid: same in units of MajorEvery cells, slightly wider lines.
    float2 cellM     = cell / MajorEvery;
    float2 dCellM    = dCell / MajorEvery;
    float2 pxMajor   = abs(frac(cellM + 0.5) - 0.5) / dCellM;
    float  majorLine = 1.0 - saturate(min(pxMajor.x, pxMajor.y) / 1.2);
    float  majorFade = saturate(2.0 - 8.0 * max(dCellM.x, dCellM.y));

    // World axes (they fade naturally as their pixel distance explodes at grazing
    // angles).
    float axisXLine = 1.0 - saturate(abs(cell.y) / dCell.y / 1.5);
    float axisZLine = 1.0 - saturate(abs(cell.x) / dCell.x / 1.5);

    float distFade = 1.0 - smoothstep(FadeDistance * 0.5, FadeDistance, tGrid);

    // Strongest contributor wins (axis > major > minor at equal strength).
    float aMinor = minorLine * minorFade * MinorColor.a;
    float aMajor = majorLine * majorFade * MajorColor.a;
    float aAxisX = axisXLine * AxisXColor.a;
    float aAxisZ = axisZLine * AxisZColor.a;

    float3 rgb = MinorColor.rgb;
    float  a   = aMinor;
    if (aMajor > a) { rgb = MajorColor.rgb; a = aMajor; }
    if (aAxisX > a) { rgb = AxisXColor.rgb; a = aAxisX; }
    if (aAxisZ > a) { rgb = AxisZColor.rgb; a = aAxisZ; }

    a *= visible * distFade;
    return float4(rgb * a, a);
}
