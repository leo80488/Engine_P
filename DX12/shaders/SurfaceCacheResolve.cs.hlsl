// Bake scratch MRTs -> material surface-cache atlas pages. Two entry points, both
// dispatched by SurfaceCacheBakePass right after the page's UV-space raster:
//   main     - repack and copy the covered texels into the page.
//   CSDilate - fill the unrasterised gutter so bilinear taps at a UV seam do not
//              fetch black.
//
// Why a resolve pass: GBuffer RT2 is (roughness, metalness, ao, reflectance), but the
// RT proxy reads surfaceTexIdx as (AO, roughness, metalness); HybridReflIntersectHW
// does
//     hitAO = surf.r;  hitRough = lerp(rMin,rMax,surf.g);  hitMetal = lerp(...,surf.b);
// so RT2 must be repacked. The same pass copies RT0, which the bake renders as
// RGBA16F because the real RT0 (R11G11B10_FLOAT) has no alpha for opacity.
//
// Coverage: RT5 (the HdrSceneColor / emissive target) is float4(s.emissive, 1.0) for
// every shaded fragment and is cleared to 0, so its alpha is an exact 0/1 coverage
// mask whatever the material authors (unlike albedo, where black is legal, or
// opacity, where 0 is legal). That channel carries both the emissive luminance and
// the coverage bit, so only three of the six scratch targets are bound.

// ==== Shared compute root signature (RHI::Slots::Cmp) ====
cbuffer ResolveCB : register(b0, space2)
{
    uint2 gPageOrigin;   // atlas texel origin of this page
    uint2 gPageSize;     // page extent in texels (== bake scratch extent)
    uint  gDilateRings;  // gutter push radius, in texels
    uint3 _pad;
};

Texture2D<float4> gBakeAlbedo   : register(t0, space2); // RT0: rgb = linear albedo, a = opacity
Texture2D<float4> gBakeSurface  : register(t1, space2); // RT2: (rough, metal, ao, reflectance)
Texture2D<float4> gBakeEmissive : register(t2, space2); // RT5: rgb = emissive, a = COVERAGE

RWTexture2D<float4> gAlbedoAtlas  : register(u0, space2);
RWTexture2D<float4> gSurfaceAtlas : register(u1, space2);

// Max rings the loop is compiled for. gDilateRings breaks out early; the
// constant bound keeps the loop unrollable and the cost bounded.
#define SC_MAX_DILATE_RINGS 4

#include "core/color.hlsli"   // Luminance

// Emissive is stored as a luminance mask, not a colour: the proxy carries a
// constant emissive.rgb per instance and the page modulates it, so a coloured
// emissive texture loses its hue. A third atlas would keep it, at 16 MB; not worth
// it until content needs it.
float SC_EmissiveMask(float3 emissive)
{
    return saturate(Luminance(emissive));
}

// RT2 (rough, metal, ao, refl) -> surface page (AO, rough, metal, emissiveMask).
float4 SC_PackSurface(float4 rt2, float3 emissive)
{
    return float4(rt2.b, rt2.r, rt2.g, SC_EmissiveMask(emissive));
}

// ==== main: one thread per page texel ====
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gPageSize.x || tid.y >= gPageSize.y) return;

    const int3   src = int3(int2(tid.xy), 0);
    const uint2  dst = gPageOrigin + tid.xy;
    const float4 emi = gBakeEmissive.Load(src);

    if (emi.a < 0.5)
    {
        // Unrasterised: write a defined value, not the previous tenant's texels; an
        // evicted page is reused by a different material and stale bleed would look like a
        // bake bug. CSDilate overwrites what it can reach; the rest is deep gutter that no
        // bilinear tap from a covered texel lands on.
        gAlbedoAtlas [dst] = float4(0.0, 0.0, 0.0, 0.0);
        gSurfaceAtlas[dst] = float4(1.0, 1.0, 0.0, 0.0);   // ao 1, rough 1, metal 0
        return;
    }

    // rgb already is baseColor x texture x whatever the hook computed, in linear
    // space (EvaluateMaterial ran in full). DDGISceneAS sets the proxy's baseColor /
    // rough / metal remaps to the identity for a paged instance, so nothing applies
    // twice.
    gAlbedoAtlas [dst] = gBakeAlbedo.Load(src);
    gSurfaceAtlas[dst] = SC_PackSurface(gBakeSurface.Load(src), emi.rgb);
}

// ==== CSDilate: gutter fill ====
// A gather (each uncovered texel searches outward for the nearest covered ring in
// the scratch targets), not the textbook iterative push: the push would read and
// write the atlas in one dispatch, and UAV read-after-write across thread groups has
// no ordering guarantee. The gather reads only SRVs and writes texels no other
// thread touches, so one dispatch is deterministic.
// Covered texels return at once; only gutter texels pay.
[numthreads(8, 8, 1)]
void CSDilate(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gPageSize.x || tid.y >= gPageSize.y) return;

    const int2 p = int2(tid.xy);
    if (gBakeEmissive.Load(int3(p, 0)).a >= 0.5) return;   // covered: main owns it

    const int2 extent = int2(gPageSize);

    [loop]
    for (uint r = 1; r <= SC_MAX_DILATE_RINGS; ++r)
    {
        if (r > gDilateRings) break;

        float4 accA = float4(0, 0, 0, 0);
        float4 accS = float4(0, 0, 0, 0);
        float  wsum = 0.0;

        const int ri = int(r);
        for (int dy = -ri; dy <= ri; ++dy)
        {
            for (int dx = -ri; dx <= ri; ++dx)
            {
                // Chebyshev ring only: the interior was tested by a nearer iteration and must
                // not out-vote it.
                if (max(abs(dx), abs(dy)) != ri) continue;

                const int2 q = p + int2(dx, dy);
                if (q.x < 0 || q.y < 0 || q.x >= extent.x || q.y >= extent.y) continue;

                const float4 e = gBakeEmissive.Load(int3(q, 0));
                if (e.a < 0.5) continue;

                accA += gBakeAlbedo.Load(int3(q, 0));
                accS += SC_PackSurface(gBakeSurface.Load(int3(q, 0)), e.rgb);
                wsum += 1.0;
            }
        }

        if (wsum > 0.0)
        {
            const uint2 dst = gPageOrigin + tid.xy;
            gAlbedoAtlas [dst] = accA / wsum;
            gSurfaceAtlas[dst] = accS / wsum;
            return;
        }
    }
}
