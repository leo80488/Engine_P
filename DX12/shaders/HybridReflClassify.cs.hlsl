// HybridReflClassify.cs.hlsl - per-8x8-tile SW/HW classification + ray-list compaction
// (port of AMD FFX ffx_classifier_reflections.h ClassifyTiles).
//
// One thread group = one 8x8 tile, dispatched ((screenW+7)/8, (screenH+7)/8).
// Thread 0 of each tile runs the hit-counter hysteresis (when both tiers are live) to
// pick FULL_SW / HALF_SW / FULL_HW, then every eligible pixel is routed to exactly one
// ray list. When the RT tier is live, deterministic mirror pixels always use HW:
// NRDResolve keeps their raw full-resolution trace, so mixing approximate Hi-Z and exact
// RayQuery landings would show the tile boundary as a sharp tear.
//   g_SWRayList - Hi-Z screen-space rays (IntersectSW)
//   g_HWRayList - DXR inline rays        (IntersectHW)
// Pixels with no ray get OutRawColor/OutHitDist zeroed here, so every on-screen pixel
// is written exactly once per frame (classify OR intersect).
//
// Deliberate deviations from the AMD reference:
//   - No samples_per_quad / temporal-variance guiding / denoiser tile list: NRD denoises
//     full-screen, every eligible pixel traces every frame.
//   - No 32-multiple helper-ray padding: the intersect passes guard on the
//     HR_CTR_*_HIST count; the padding is only an atomic-coalescing optimization.
//   - No FillEnvironment fallback: untraced pixels are zeroed and the resolve /
//     Lighting.ps probe+sky tier owns them (the engine's 3-tier composite).

#include "HybridReflCommon.hlsli"

Texture2D<float>    gDepth          : register(t2,  space0);
Texture2D<float4>   gSurface        : register(t4,  space0);   // .r = perceptual roughness
Texture2D<float2>   gVelocity       : register(t5,  space0);   // raw NDC delta curNDC-prevNDC
Texture2D<uint>     gHitCounterHist : register(t31, space0);   // last frame's tile hit-counter

RWTexture2D<float4>      OutRawColor   : register(u0, space0);
RWTexture2D<float>       OutHitDist    : register(u1, space0);
RWStructuredBuffer<uint> g_SWRayList   : register(u2, space0);
RWStructuredBuffer<uint> g_HWRayList   : register(u3, space0);
RWStructuredBuffer<uint> g_RayCounter  : register(u4, space0);
RWTexture2D<uint>        OutHitCounter : register(u6, space0);

// ==== FFX classifier RNG (ported verbatim from ffx_classifier_reflections.h) ====

float2 HR_Hash22(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx + p3.yz) * p3.zy);
}

float2 HR_GetRandom(uint2 index)
{
    float2 pos = float2(index) * 0.152 + float(frameIndex) / 60.0 * 1500.0 + 50.0;
    return HR_Hash22(pos);
}

float2 HR_GetRandomLastFrame(uint2 index)
{
    float2 pos = float2(index) * 0.152 + float(frameIndex - 1u) / 60.0 * 1500.0 + 50.0;
    return HR_Hash22(pos);
}

// Turn a random tile full hybrid once in a while to keep testing Hi-Z traversal.
bool HR_IsSW(float hits, float misses, float rnd)
{
    return rnd <= (hybridSpawnRate + hits - misses * hybridMissWeight);
}

// ==== Classification kernel ====

groupshared uint gsTileClass;
groupshared uint gsSWCount;
groupshared uint gsSWBase;

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID,
          uint3 GTid : SV_GroupThreadID,
          uint3 Gid  : SV_GroupID)
{
    const uint2 pixel  = DTid.xy;
    const uint2 gtid   = GTid.xy;
    const uint2 tile   = Gid.xy;
    const uint  flatId = gtid.y * 8u + gtid.x;

    // Effective tier flags. The SW tier needs a Hi-Z pyramid, a valid
    // prev-frame HDR to shade hits from, and live history.
    const bool swOn = ((unifiedFlags & 1u) != 0u) && hizMipCount > 0u
                   && hasPrevHdr != 0u && resetHistory == 0u;
    const bool rtOn = (unifiedFlags & 2u) != 0u;

    // ---- Tile class + hit-counter shift (thread 0) ----
    if (flatId == 0u)
    {
        gsSWCount = 0u;

        uint tileClass = swOn ? HR_TILE_CLASS_FULL_SW : HR_TILE_CLASS_FULL_HW;
        uint hist      = 0u;   // counter whose fresh bytes we roll into the old bytes

        if (swOn && rtOn)
        {
            // Hit-counter hysteresis (FFX ClassifyTiles).
            // History layout on read (see HybridReflCommon.hlsli):
            //   bits 0..7  /16..23 = fresh hits/misses accumulated by IntersectSW during the
            //                        PREVIOUS frame
            //   bits 8..15 /24..31 = the frame before that ("old")
            // resetHistory == 0 is guaranteed here (swOn requires it).

            // Reproject last frame's stats through the motion of a random pixel inside this tile,
            // which keeps coherence under movement.
            const float2 xi      = HR_GetRandom(tile);
            const int2   mix     = int2(xi * 8.0);
            const uint2  probePx = min(tile * 8u + uint2(mix),
                                       uint2(screenW - 1u, screenH - 1u));
            const float2 vel     = gVelocity.Load(int3(probePx, 0));
            const float2 uv8     = (float2(probePx) + 0.5) * float2(invScreenW, invScreenH);
            const float2 prevUV  = HR_PrevUVFromVelocity(uv8, vel);

            const int2 histMax = int2(int(tileCountX) - 1, int(tileCountY) - 1);
            int2 reprojTile = int2(prevUV * float2(screenW, screenH) / 8.0);
            reprojTile = clamp(reprojTile, int2(0, 0), histMax);
            uint hitcounter = gHitCounterHist.Load(int3(reprojTile, 0));

            // 3x3 safe band: take the neighbourhood counter with the biggest SW success rate; hides
            // artefacts (geometry missing from the BVH, fireflies) at tile borders during movement.
            uint samePixel = 0u;
            const int radius = 1;
            for (int y = -radius; y <= radius; ++y)
            {
                for (int x = -radius; x <= radius; ++x)
                {
                    const int2 t  = clamp(int2(tile) + int2(x, y), int2(0, 0), histMax);
                    const uint pt = gHitCounterHist.Load(int3(t, 0));
                    if ((pt & 0xFFu) > (samePixel & 0xFFu))
                        samePixel = pt;
                }
            }

            // Compare with the reprojected counter, keep the bigger success rate.
            if ((hitcounter & 0xFFu) < (samePixel & 0xFFu))
                hitcounter = samePixel;

            const float rnd     = xi.x;                            // == GetRandom(tile).x
            const float rndLast = HR_GetRandomLastFrame(tile).x;
            const float hitsNew = float(hitcounter & 0xFFu);           // fresh (prev frame)
            const float hitsOld = float(HR_Hitcounter_GetSWHits(hitcounter));   // bits 8..15
            const float missNew = float((hitcounter >> 16u) & 0xFFu);  // fresh (prev frame)
            const float missOld = float(HR_Hitcounter_GetSWMisses(hitcounter)); // bits 24..31
            const bool  newClass = HR_IsSW(hitsNew, missNew, rnd);
            const bool  oldClass = HR_IsSW(hitsOld, missOld, rndLast);

            // Extra checkerboard stage when the class flips, to soften the pop.
            if (newClass == oldClass)
                tileClass = newClass ? HR_TILE_CLASS_FULL_SW : HR_TILE_CLASS_FULL_HW;
            else
                tileClass = HR_TILE_CLASS_HALF_SW;

            hist = hitcounter;
        }

        gsTileClass = tileClass;

        // Roll last frame's fresh bytes into the 'old' bytes; IntersectSW atomically adds
        // HR_HITCOUNTER_SW_HIT/MISS_FLAG into the (zeroed) low bytes during this frame. Written
        // for EVERY tile, even single-tier (hist == 0), so stale counters never leak back in when
        // the second tier turns on.
        OutHitCounter[tile] = (min(hist & 0xFFu, 255u) << 8u)
                            | (min((hist >> 16u) & 0xFFu, 255u) << 24u);
    }
    GroupMemoryBarrierWithGroupSync();   // gsTileClass + gsSWCount=0 visible

    // ---- Per-pixel ray decision ----
    const bool  onScreen  = (pixel.x < screenW) && (pixel.y < screenH);
    const float depth     = gDepth.Load(int3(pixel, 0));
    const bool  isSurface = depth > 0.0;               // reverse-Z: sky is EXACTLY 0
    const float roughness = gSurface.Load(int3(pixel, 0)).r;
    const bool  needsRay  = onScreen && isSurface && (roughness < roughnessThreshold);

    // Route: HALF_SW keeps the checkerboard half on the screen tracer.
    // When !swOn everything lands on HW; when !rtOn everything lands on SW.
    //
    // The deterministic mirror band is HW-only in hybrid mode: its resolve bypasses REBLUR,
    // so no spatial filter hides the landing-position difference between approximate Hi-Z
    // and exact RayQuery hits, and a FULL_SW/HALF_SW tile transition would show as
    // horizontal chunks of a reflected mesh. Glossy pixels above mirrorRoughness keep the
    // normal hybrid routing.
    const bool checkerboard = (((gtid.x ^ gtid.y) & 1u) == 0u);
    const bool forceMirrorHW = rtOn && roughness <= mirrorRoughness;
    const bool needsSW = needsRay && swOn && !forceMirrorHW &&
        (gsTileClass == HR_TILE_CLASS_FULL_SW ||
         (gsTileClass == HR_TILE_CLASS_HALF_SW && checkerboard));
    const bool needsHW = needsRay && rtOn && !needsSW;

    // ---- Compaction (FFX wave scheme, minus the 32-multiple padding) ----
    // All barriers stay at top level: wave ops here run with every lane of the group alive
    // (no early returns above).

    // SW: two-level (wave -> groupshared -> global) so the tile's rays stay contiguous in
    // the list.
    const uint swWaveCount  = WaveActiveCountBits(needsSW);
    const uint swLanePrefix = WavePrefixCountBits(needsSW);
    uint swWaveBase = 0u;
    if (WaveIsFirstLane())
        InterlockedAdd(gsSWCount, swWaveCount, swWaveBase);
    swWaveBase = WaveReadLaneFirst(swWaveBase);

    // HW: per-wave straight to the global counter.
    const uint hwWaveCount  = WaveActiveCountBits(needsHW);
    const uint hwLanePrefix = WavePrefixCountBits(needsHW);
    uint hwWaveBase = 0u;
    if (WaveIsFirstLane() && hwWaveCount)
        InterlockedAdd(g_RayCounter[HR_CTR_HW_COUNT], hwWaveCount, hwWaveBase);
    hwWaveBase = WaveReadLaneFirst(hwWaveBase);

    GroupMemoryBarrierWithGroupSync();   // gsSWCount final

    if (flatId == 0u)
    {
        uint tileBase;
        InterlockedAdd(g_RayCounter[HR_CTR_SW_COUNT], gsSWCount, tileBase);
        gsSWBase = tileBase;
    }
    GroupMemoryBarrierWithGroupSync();   // gsSWBase visible

    if (needsSW)
        g_SWRayList[gsSWBase + swWaveBase + swLanePrefix] = HR_PackRay(pixel);
    else if (needsHW)
        g_HWRayList[hwWaveBase + hwLanePrefix] = HR_PackRay(pixel);

    // ---- No ray -> this pass owns the pixel's outputs ----
    if (onScreen && !needsSW && !needsHW)
    {
        OutRawColor[pixel] = 0;
        OutHitDist[pixel]  = 0;
    }
}
