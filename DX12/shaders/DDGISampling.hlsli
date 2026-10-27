// DDGISampling.hlsli - sample DDGI at shading time (Lighting.ps, the reflection,
// SSGI / RTGI and DDGI trace shaders).
//
// Irradiance storage: octahedral atlas (Texture2D<float4>, RGBA16F),
//                     6x6 texels + 1-texel mirrored border per probe.
// Depth storage:      octahedral atlas (Texture2D<float2>, mean + variance) for Chebyshev.
//
// Both atlases have the same shape and are sampled the same way (DDGI_TileUV ->
// bilinear): one addressing convention, one border kernel, one class of seam bug.
// Trilinear-with-Chebyshev visibility per Majercik 2019 / RTXGI.
//
// Per-probe irradiance is a 36-texel bilinear tap, not L1 SH (see DDGICommon.hlsli,
// per-probe irradiance storage): L1's four coefficients per channel leave all finer
// directional detail to inter-probe interpolation, which the perceptual blend
// darkens and probe spacing bounds.

#ifndef DDGI_SAMPLING_HLSLI
#define DDGI_SAMPLING_HLSLI

#include "DDGICommon.hlsli"

// Direction -> UV inside a probe's octahedral atlas tile (the +1 texel border
// included).
float2 DDGI_TileUV(float3 dir, uint probeSize, uint probeStride)
{
    float2 oct = OctEncode(dir) * 0.5 + 0.5; // [0,1]
    float2 tex = oct * float(probeSize) + 1.0;
    return tex / float(probeStride);
}

// Sample one probe's depth atlas tile via bilinear interpolation along the
// octahedral mapping of `dir`.
float2 DDGI_SampleProbeDepthUV(Texture2D<float2> atlas,
                               SamplerState sampLinear,
                               int3 probeCoord,
                               DDGIVolumeGPU vol,
                               float3 dir)
{
    const uint probeSize   = DDGI_DEPTH_PROBE_SIZE;
    const uint probeStride = DDGI_DEPTH_PROBE_STRIDE;
    uint2  tile = DDGI_ProbeAtlasTile(probeCoord, vol, probeStride);
    float2 tileUV = DDGI_TileUV(dir, probeSize, probeStride);

    uint w, h;
    atlas.GetDimensions(w, h);
    float2 uv = (float2(tile) + tileUV * float(probeStride)) / float2(w, h);
    return atlas.SampleLevel(sampLinear, uv, 0).rg;
}

// Sample one probe's IRRADIANCE atlas tile via bilinear interpolation along
// the octahedral mapping of `dir` (the shading normal at the receiving point).
//
// Deliberately identical in shape to DDGI_SampleProbeDepthUV: same DDGI_TileUV,
// same tile origin, same normalisation, because the two atlases share
// DDGI_ProbeAtlasTile and DDGIBorderUpdate. A convention change here must move
// the border kernel and the relight write site with it; they are one system.
//
// The +1 border that DDGI_TileUV skips into makes the hardware bilinear legal at
// the octahedral seam: DDGIBorderUpdate fills the ring with the mirrored opposite
// edge, so a tap straddling the tile boundary blends against the direction that
// is adjacent ON THE SPHERE, not against the neighbouring probe's texels.
//
// Returns LINEAR irradiance (E/pi). Stored texels are finite and non-negative
// (DDGIRelight sanitises at the write site) and bilinear interpolation preserves
// both, so there is no read-site clamp.
float3 DDGI_SampleProbeIrradiance(Texture2D<float4> atlas,
                                  SamplerState sampLinear,
                                  int3 probeCoord,
                                  DDGIVolumeGPU vol,
                                  float3 dir)
{
    const uint probeSize   = DDGI_IRRADIANCE_PROBE_SIZE;
    const uint probeStride = DDGI_IRRADIANCE_PROBE_STRIDE;
    uint2  tile   = DDGI_ProbeAtlasTile(probeCoord, vol, probeStride);
    float2 tileUV = DDGI_TileUV(dir, probeSize, probeStride);

    uint w, h;
    atlas.GetDimensions(w, h);
    float2 uv = (float2(tile) + tileUV * float(probeStride)) / float2(w, h);
    return atlas.SampleLevel(sampLinear, uv, 0).rgb;
}

// Spherical-average irradiance of one probe tile: 4 bilinear octahedral taps at
// the vertices of a regular tetrahedron, averaged. Those 4 directions integrate
// the SH band-0 (DC) term exactly and cancel band 1, so the result is a stable
// estimate of the probe's average E/pi over the whole sphere.
//
// For stylized (toon) receivers: their form model is the authored ramp/SDF, and
// evaluating the probe at the per-pixel vertex normal re-adds a second form
// gradient whose C1 creases at triangle edges read as polygonal steps once direct
// light is scaled down. Do not use a fixed direction instead: (0,1,0) returns the
// zenith of whatever the probes see (under a Rayleigh sky: strongly blue), not
// the ambient average.
float3 DDGI_ProbeIrradianceIsotropic(Texture2D<float4> atlas,
                                     SamplerState sampLinear,
                                     int3 probeCoord,
                                     DDGIVolumeGPU vol)
{
    const float k = 0.5773502691896258; // 1/sqrt(3)
    float3 irr = DDGI_SampleProbeIrradiance(atlas, sampLinear, probeCoord, vol, float3( k,  k,  k));
    irr       += DDGI_SampleProbeIrradiance(atlas, sampLinear, probeCoord, vol, float3( k, -k, -k));
    irr       += DDGI_SampleProbeIrradiance(atlas, sampLinear, probeCoord, vol, float3(-k,  k, -k));
    irr       += DDGI_SampleProbeIrradiance(atlas, sampLinear, probeCoord, vol, float3(-k, -k,  k));
    return irr * 0.25;
}

// Core sampler: RAW physical irradiance, no per-volume tint/scale. Returns rgb =
// irradiance, a = the volume's fade weight in [0..1] (0 when no probe survives);
// the caller falls back to sky IBL diffuse for the (1 - a) remainder.
//
// `normal` is ALWAYS the real surface normal: the leak-prevention bias, the
// backface probe weight and the Chebyshev setup are geometric visibility
// questions and must not be bent by shading stylization. `isotropicIrradiance`
// only switches WHAT is read from the surviving probes: the directional
// octahedral tap at `normal` (physical) or DDGI_ProbeIrradianceIsotropic
// (stylized ambient). Every call site passes a literal, so DXC folds the branch.
//
// `gatherSky` returns in `sky` the scalar sky visibility with trilinear-only
// weights. `gatherMoments` averages the probes' sky-visibility moments (v0 =
// skyVisibility, v1 = skyVisDir) with the SAME per-probe weight as the irradiance
// (backface x Chebyshev x crush x trilinear), so they come from exactly the probes
// the irradiance trusts: with trilinear-only weights a probe under a floor slab
// sees its misses point down and turns the floor's "sky is above" upside down.
float4 DDGI_SampleVolumeRawExWithSky(float3 worldPos,
                              float3 normal,
                              float3 viewDir,
                              DDGIVolumeGPU vol,
                              Texture2D<float4> irradianceAtlas,
                              Texture2D<float2> depthAtlas,
                              StructuredBuffer<DDGIProbeData> probeData,
                              SamplerState sampLinear,
                              uniform bool isotropicIrradiance,
                              uniform bool gatherSky, out float2 sky,
                              uniform bool gatherMoments, out float4 skyMoments)
{
    sky = float2(1,0);
    skyMoments = float4(1, 0, 0, 0);
    float skySum = 0, skyWeight = 0;
    float4 momentSum = float4(0, 0, 0, 0);
    float volumeWeight = DDGI_VolumeFadeWeight(worldPos, vol);
    if (volumeWeight <= 1e-4) return float4(0, 0, 0, 0);

    float3 biasedPos = worldPos + normal * vol.normalBias + viewDir * vol.viewBias;

    float3 g     = DDGI_GridSpace(biasedPos, vol);
    int3   base  = int3(floor(g));

    base.x = clamp(base.x, 0, (int)vol.probeCountsX - 2);
    base.y = clamp(base.y, 0, (int)vol.probeCountsY - 2);
    base.z = clamp(base.z, 0, (int)vol.probeCountsZ - 2);

    // Trilinear alpha AGAINST THE CLAMPED BASE, not frac(g). With frac(), a biased
    // position pushed past the first/last probe plane (normalBias / viewBias do this
    // near the AABB faces) wraps: g = counts-1+eps gives base = counts-2 (clamped) but
    // alpha ~ 0, snapping the interpolation a full cell backwards - a hard
    // probe-cell-shaped discontinuity. With saturate(g - base) the edge cells
    // extrapolate flat instead.
    float3 alpha = saturate(g - float3(base));

    // Two accumulators: the linear weighted mean and the perceptual (gamma-2 /
    // sqrt-space) one. The reference algorithm uses only the second, but it is a
    // power mean with exponent 1/2, so it is always <= the linear mean, with the
    // deficit growing with the spread of the 8 probe values (~21% at a 10x spread,
    // ~40% at 100x): a smooth dark lobe across a hard sun/shadow boundary that denser
    // probes shrink but never remove. vol.perceptualBlendWeight dials between them for
    // one extra madd per tap and no transcendentals.
    float3 sumIrrLin  = 0;
    float3 sumIrrPerc = 0;
    float  sumW   = 0;

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        int3 off = int3((i >> 0) & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 c   = base + off;

        if (any(c < 0) ||
            c.x >= (int)vol.probeCountsX ||
            c.y >= (int)vol.probeCountsY ||
            c.z >= (int)vol.probeCountsZ) continue;

        uint probeIdx = DDGI_ProbeIndex(c, vol);
        DDGIProbeData pd = probeData[probeIdx];
        // Packed-state aware (DDGICommon.hlsli): skips INACTIVE probes and, for one
        // frame, freshly reactivated (FRESH) ones whose held mean is stale until the
        // relight seed lands.
        if (DDGI_ProbeSkipSampling(pd.state)) continue;
        // Same cell and data load as the irradiance; the sky term keeps its own
        // trilinear weights (no Chebyshev / backface weighting).
        if (gatherSky)
        {
            float3 tri = lerp(1.0-alpha, alpha, float3(off));
            float sw = max(tri.x*tri.y*tri.z, 1e-3);
            skySum += saturate(pd.skyVisibility)*sw;
            skyWeight += sw;
        }

        float3 probePos = DDGI_ProbeWorldPos(c, vol) + pd.offset;
        float3 dirToProbe = probePos - biasedPos;
        float  distToProbe = length(dirToProbe);
        // Degenerate case: the shading point coincides with the probe. Keep the distance
        // tiny: a larger absolute value (e.g. 1 world unit) could exceed the probe's depth
        // mean and falsely Chebyshev-occlude the probe we are standing on.
        if (distToProbe < 1e-4) { dirToProbe = normal; distToProbe = 1e-4; }
        else                    { dirToProbe /= distToProbe; }

        // ---- Per-probe visibility weight (backface x Chebyshev) ----
        // Built separately from the trilinear factor so the crush remap below acts on
        // visibility alone (Majercik 2019 / RTXGI ordering).
        //
        // Backface: smooth wrap-shading falloff, sq(bf) + 0.2. The additive 0.2 keeps
        // behind-ish probes contributing a little, smoother across cell faces than a hard
        // floor.
        float bf = (dot(dirToProbe, normal) + 1.0) * 0.5;
        float w  = bf * bf + 0.2;

        // Chebyshev visibility via the depth atlas. Skip the test while the atlas is
        // unwritten (mean ~ 0): with mean = 0 and any real probe distance the formula
        // gives occlusion 0, so a freshly allocated volume would read fully occluded
        // until depth converges (~3 overwrite frames). Trilinear + backface alone are
        // trusted for that warm-up.
        float2 d = DDGI_SampleProbeDepthUV(depthAtlas, sampLinear, c, vol, -dirToProbe);
        float mean = d.x;
        if (mean > 1e-3 && distToProbe > mean)
        {
            // The atlas stores (mean, VARIANCE): the relight CS subtracts E[d^2] - E[d]^2 in
            // fp32 before the fp16 store. Do not compute it here from two fp16 moments: it
            // cancels catastrophically (ULP(mean^2) ~ 0.016 at mean ~ 5), and the noise, cubed
            // into the weight below, makes near-knife-edge probes pop in and out of the blend.
            float variance = max(d.y, 0.0);
            float chebDist = distToProbe - mean;
            float occlusion = variance / (variance + chebDist * chebDist);
            // Floor 0.05: never let visibility reach zero. When every probe of a cell is
            // occluded the renormalization would amplify whichever probe numerically survives,
            // painting hard probe-shaped blocks in shadowed corners (the reference
            // implementation keeps the same floor).
            w *= max(occlusion * occlusion * occlusion, 0.05);
        }

        // Crush tiny visibility weights smoothly toward zero (reference implementation):
        // the renormalization divides by sumW, so a probe that barely passed backface +
        // Chebyshev would be amplified back to full contribution wherever it is the
        // "least bad" probe - the main source of blocky light/dark cells in dark areas.
        const float kCrush = 0.2;
        if (w < kCrush) w *= (w * w) / (kCrush * kCrush);

        // Trilinear factor last, floored so a shading point exactly on a probe plane
        // cannot zero the whole cell's weight sum.
        float3 trilinear = lerp(1.0 - alpha, alpha, float3(off));
        w *= max(trilinear.x * trilinear.y * trilinear.z, 1e-3);

        // Irradiance: bilinear octahedral tap at the surface normal, accumulated in BOTH
        // linear and sqrt space (gamma 2). Linear blending between a dark and a lit probe
        // stays perceptually bright too long, so probe-cell boundaries pop in dark
        // regions; interpolating sqrt(E) is the paper's fix for visible seams, but it is a
        // power mean and darkens high-contrast cells, hence vol.perceptualBlendWeight.
        //
        // The irradiance tap uses `normal` (where the receiving surface faces), or the
        // tetrahedral average in isotropic mode, while the Chebyshev tap uses
        // `-dirToProbe` (where the probe sees the shading point from). They are different
        // questions; do not unify them.
        float3 irr = isotropicIrradiance
                   ? DDGI_ProbeIrradianceIsotropic(irradianceAtlas, sampLinear, c, vol)
                   : DDGI_SampleProbeIrradiance(irradianceAtlas, sampLinear,
                                                c, vol, normal);
        sumIrrLin  += irr * w;
        sumIrrPerc += sqrt(irr) * w;
        sumW       += w;
        if (gatherMoments)
            momentSum += float4(saturate(pd.skyVisibility), pd.skyVisDir) * w;
    }

    if (gatherSky && skyWeight > 1e-4) sky = float2(saturate(skySum/skyWeight), volumeWeight);
    if (sumW < 1e-4) return float4(0, 0, 0, 0);
    if (gatherMoments) skyMoments = momentSum / sumW;

    const float  invSumW = 1.0 / sumW;
    const float3 meanLin = sumIrrLin * invSumW;
    float3 meanPerc      = sumIrrPerc * invSumW;
    meanPerc            *= meanPerc;   // undo the sqrt-space (gamma 2) blend

    // 1 = reference behaviour (pure perceptual blend, smooth in dark regions),
    // 0 = pure linear (no contrast darkening). saturate() so a stale or garbage
    // volume descriptor cannot extrapolate past the two endpoints.
    const float3 irradiance =
        lerp(meanLin, meanPerc, saturate(vol.perceptualBlendWeight));

    return float4(irradiance, volumeWeight);
}

// Irradiance only: sky and moment gathering compiled out.
float4 DDGI_SampleVolumeRawEx(float3 worldPos, float3 normal, float3 viewDir,
    DDGIVolumeGPU vol, Texture2D<float4> irradianceAtlas, Texture2D<float2> depthAtlas,
    StructuredBuffer<DDGIProbeData> probeData, SamplerState sampLinear,
    uniform bool isotropicIrradiance)
{
    float2 unusedSky;
    float4 unusedMoments;
    return DDGI_SampleVolumeRawExWithSky(worldPos, normal, viewDir, vol,
        irradianceAtlas, depthAtlas, probeData, sampLinear, isotropicIrradiance, false, unusedSky,
        false, unusedMoments);
}

// Physical/default entry: raw directional irradiance at the surface normal, no
// per-volume tint/scale. Multi-bounce feedback (DDGIRayTrace) and the SSGI / RTGI
// traces must use this variant: tint and diffuseScale are display knobs, and in
// the bounce loop they multiply once per bounce, make the series gain
// artist-dependent and can push it past 1 (diffuseScale 2 doubles it: a
// non-convergent brightness runaway).
float4 DDGI_SampleVolumeRaw(float3 worldPos,
                            float3 normal,
                            float3 viewDir,
                            DDGIVolumeGPU vol,
                            Texture2D<float4> irradianceAtlas,
                            Texture2D<float2> depthAtlas,
                            StructuredBuffer<DDGIProbeData> probeData,
                            SamplerState sampLinear)
{
    return DDGI_SampleVolumeRawEx(worldPos, normal, viewDir, vol,
                                  irradianceAtlas, depthAtlas, probeData,
                                  sampLinear, false);
}

// Display-path entry: raw sample x per-volume tint/scale. For Lighting.ps and
// any final-shading consumer; the bounce loops call DDGI_SampleVolumeRaw.
float4 DDGI_SampleVolume(float3 worldPos,
                         float3 normal,
                         float3 viewDir,
                         DDGIVolumeGPU vol,
                         Texture2D<float4> irradianceAtlas,
                         Texture2D<float2> depthAtlas,
                         StructuredBuffer<DDGIProbeData> probeData,
                         SamplerState sampLinear)
{
    float4 s = DDGI_SampleVolumeRaw(worldPos, normal, viewDir, vol,
                                    irradianceAtlas, depthAtlas, probeData, sampLinear);
    s.rgb *= vol.diffuseTint * vol.diffuseScale;
    return s;
}

// Display-path ISOTROPIC entry for toon / stylized receivers. Same geometry (bias,
// backface, Chebyshev, all on the real normal), spherical-average irradiance tap.
// Never part of the DDGI feedback loop: it is a receiving-surface stylization only.
float4 DDGI_SampleVolumeAmbientIso(float3 worldPos,
                                   float3 normal,
                                   float3 viewDir,
                                   DDGIVolumeGPU vol,
                                   Texture2D<float4> irradianceAtlas,
                                   Texture2D<float2> depthAtlas,
                                   StructuredBuffer<DDGIProbeData> probeData,
                                   SamplerState sampLinear)
{
    float4 s = DDGI_SampleVolumeRawEx(worldPos, normal, viewDir, vol,
                                      irradianceAtlas, depthAtlas, probeData,
                                      sampLinear, true);
    s.rgb *= vol.diffuseTint * vol.diffuseScale;
    return s;
}

// Display-path entry plus the sky-visibility moments (see the core sampler's
// `gatherMoments`): what DDGI_SampleVolume / _AmbientIso return (`isotropic`
// picks which - a literal at every call site), and in `skyMoments` the probes'
// (v0, v1) averaged with the irradiance's own visibility weights. (1, 0, 0, 0)
// when no probe survives - the caller lerps by the returned coverage (.a) anyway.
float4 DDGI_SampleVolumeSkyMoments(float3 worldPos,
                                   float3 normal,
                                   float3 viewDir,
                                   DDGIVolumeGPU vol,
                                   Texture2D<float4> irradianceAtlas,
                                   Texture2D<float2> depthAtlas,
                                   StructuredBuffer<DDGIProbeData> probeData,
                                   SamplerState sampLinear,
                                   uniform bool isotropic,
                                   out float4 skyMoments)
{
    float2 unusedSky;
    float4 s = DDGI_SampleVolumeRawExWithSky(worldPos, normal, viewDir, vol,
        irradianceAtlas, depthAtlas, probeData, sampLinear, isotropic, false, unusedSky,
        true, skyMoments);
    s.rgb *= vol.diffuseTint * vol.diffuseScale;
    return s;
}

// ==== Multi-volume arbitration ====
//
// Overlapping volumes are ranked, not averaged: the best-ranked volume owns its
// interior and hands over to the runner-up only across its own fade band. A
// fade-weighted mean over all volumes instead
//  1. lets a dense interior volume reach at best 50/50 with the coarse volume it
//     should override (a third with three), so the coarse volume's leak survives
//     and "this room wins" cannot be authored;
//  2. sums coverage while it averages irradiance: four volumes at the edge of
//     their fades (0.25 each) report full coverage and suppress the sky-IBL
//     fallback exactly where they are worst;
//  3. lets every consumer fold coverage in its own way, and the copies drift apart
//     (folding coverage into the value makes RT reflections ~3x darker than the
//     screen).
//
// Only the ranking and the combine rule live here (pure ALU over the volume
// buffer): the consumers do not share resource access (Lighting.ps opens each
// volume's atlases and probe data through ResourceDescriptorHeap, the other
// shaders bind their own), so each caller samples the winners with its own
// bindings.
//
// Usage:
//     DDGIVolumePick pick = DDGI_PickVolumes(worldPos, volumes);
//     if (pick.bestSlot >= 0) { ...sample pick.bestSlot with YOUR bindings... }
//     if (pick.secondSlot >= 0) { ...sample pick.secondSlot... }
//     float4 r = DDGI_CombineWinners(bestSample, secondSample, pick);

struct DDGIVolumePick
{
    int   bestSlot;     // -1 = no volume covers worldPos
    float bestFade;
    int   secondSlot;   // -1 = only one volume covers worldPos
    float secondFade;
    float coverage;     // max fade over ALL covering volumes, [0..1]
};

// Arbitration score. Priority (authored) dominates; probe DENSITY breaks ties,
// because a finer grid is the more detailed description of the same space and
// should win wherever it reaches. Never compares fade -- fade decides how the
// winner blends OUT at its boundary, not who the winner is.
float DDGI_VolumeRank(DDGIVolumeGPU vol)
{
    const uint  priority   = vol.schedulingPacked & 0xFFFFu;
    const float minSpacing = min(vol.probeSpacing.x,
                             min(vol.probeSpacing.y, vol.probeSpacing.z));
    return float(priority) * 1e6 + 1.0 / max(minSpacing, 1e-3);
}

DDGIVolumePick DDGI_PickVolumes(float3 worldPos,
                                StructuredBuffer<DDGIVolumeGPU> volumes)
{
    DDGIVolumePick p;
    p.bestSlot   = -1;  p.bestFade   = 0.0;
    p.secondSlot = -1;  p.secondFade = 0.0;
    p.coverage   = 0.0;

    float bestRank   = -1e30;
    float secondRank = -1e30;

    [loop]
    for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
    {
        DDGIVolumeGPU vol = volumes[vi];
        if ((vol.flags & 1u) == 0u) continue;          // slot not allocated

        const float fade = DDGI_VolumeFadeWeight(worldPos, vol);
        if (fade <= 1e-4) continue;                    // outside this volume

        // Coverage is a MAX, not a sum: it answers "how strongly is this point
        // inside ANY volume", which is what the sky-IBL fallback wants. A sum
        // reports full coverage from several weak contributions.
        p.coverage = max(p.coverage, fade);

        const float rank = DDGI_VolumeRank(vol);
        if (rank > bestRank)
        {
            secondRank = bestRank;   p.secondSlot = p.bestSlot; p.secondFade = p.bestFade;
            bestRank   = rank;       p.bestSlot   = (int)vi;    p.bestFade   = fade;
        }
        else if (rank > secondRank)
        {
            secondRank = rank;       p.secondSlot = (int)vi;    p.secondFade = fade;
        }
    }

    p.coverage = saturate(p.coverage);
    return p;
}

// @p best / @p second are DDGI_SampleVolume results for pick.bestSlot /
// pick.secondSlot (float4(0,0,0,0) when that slot is -1).
//
// A zero .a on a picked slot means "inside this volume, but none of the eight
// surrounding probes carried usable data" (every corner INACTIVE, or the cell's
// weights collapsed). That is a no-data signal, not zero light, so it falls
// through to the runner-up; feeding it into a weighted mean would zero the weight
// and draw a hard probe-cell-shaped seam along the classification boundary.
float4 DDGI_CombineWinners(float4 best, float4 second, DDGIVolumePick pick)
{
    const bool bestOk   = (pick.bestSlot   >= 0) && (best.a   > 1e-4);
    const bool secondOk = (pick.secondSlot >= 0) && (second.a > 1e-4);

    if (!bestOk && !secondOk) return float4(0, 0, 0, 0);
    if (!bestOk)              return float4(second.rgb, pick.coverage);
    if (!secondOk)            return float4(best.rgb,   pick.coverage);

    // The winner owns its interior (bestFade == 1 there) and hands over to the
    // runner-up only across its own boundary fade band, so a dense volume nested in a
    // coarse one overrides it instead of averaging with it.
    return float4(lerp(second.rgb, best.rgb, saturate(pick.bestFade)),
                  pick.coverage);
}

// Same arbitration for DDGI_SampleSkyVisibility results (x = sky visibility,
// y = that volume's coverage).
//
// A pixel must never take its indirect diffuse from one volume and its sky
// visibility / rough-specular fallback from another: every consumer resolves all
// three from ONE DDGIVolumePick, consistent by construction.
//
// Returns x = sky visibility, y = coverage. Callers use
// `lerp(1.0, x, saturate(y))`: uncovered space is fully sky-visible.
float2 DDGI_CombineSkyVisibility(float2 best, float2 second, DDGIVolumePick pick)
{
    const bool bestOk   = (pick.bestSlot   >= 0) && (best.y   > 1e-4);
    const bool secondOk = (pick.secondSlot >= 0) && (second.y > 1e-4);

    if (!bestOk && !secondOk) return float2(1.0, 0.0);
    if (!bestOk)              return float2(second.x, pick.coverage);
    if (!secondOk)            return float2(best.x,   pick.coverage);

    return float2(lerp(second.x, best.x, saturate(pick.bestFade)),
                  pick.coverage);
}

// Loop predicate for the consumers. Each binds its per-volume resources
// differently (Lighting.ps opens them from ResourceDescriptorHeap, which needs
// CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED; the reflection shaders use register arrays),
// so the arbitration cannot own the sampling. Every consumer keeps its `[unroll]`
// over DDGI_MAX_VOLUMES (the resource index stays a compile-time constant) and
// samples only the two winners:
//
//     [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi) {
//         if (!DDGI_IsPicked(pick, vi)) continue;
//         ... sample with constant vi ...
//     }
//
// bestSlot / secondSlot are -1 when unset, and (uint)(-1) never matches a loop
// index, so the "no volume covers this point" case falls out for free.
bool DDGI_IsPicked(DDGIVolumePick pick, uint vi)
{
    return or(vi == (uint)pick.bestSlot, vi == (uint)pick.secondSlot);
}

// Per-probe open-sky probability for the indirect-specular fallback. Follows the
// irradiance sampler's volume fade, normal/view bias, relocation and trilinear
// interpolation, but deliberately omits the backface/Chebyshev tests:
// skyVisibility is already an aggregate visibility statistic, and a second hard
// visibility test would turn its smooth indoor/outdoor transition back into noisy
// probe-cell boundaries.
//
// Returns x = sky visibility, y = DDGI volume coverage. Outside the volume (or
// when no active probe survives) coverage is zero; callers use skyVis = 1 for
// that uncovered remainder.
float2 DDGI_SampleSkyVisibility(float3 worldPos,
                                float3 normal,
                                float3 viewDir,
                                DDGIVolumeGPU vol,
                                StructuredBuffer<DDGIProbeData> probeData)
{
    const float volumeWeight = DDGI_VolumeFadeWeight(worldPos, vol);
    if (volumeWeight <= 1e-4) return float2(1.0, 0.0);

    const float3 biasedPos = worldPos + normal * vol.normalBias
                           + viewDir * vol.viewBias;
    const float3 g = DDGI_GridSpace(biasedPos, vol);
    int3 base = int3(floor(g));
    base.x = clamp(base.x, 0, (int)vol.probeCountsX - 2);
    base.y = clamp(base.y, 0, (int)vol.probeCountsY - 2);
    base.z = clamp(base.z, 0, (int)vol.probeCountsZ - 2);
    const float3 alpha = saturate(g - float3(base));

    float sumSky = 0.0;
    float sumW   = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        const int3 off = int3((i >> 0) & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3 c = base + off;
        if (any(c < 0) ||
            c.x >= (int)vol.probeCountsX ||
            c.y >= (int)vol.probeCountsY ||
            c.z >= (int)vol.probeCountsZ) continue;

        const uint probeIdx = DDGI_ProbeIndex(c, vol);
        const DDGIProbeData pd = probeData[probeIdx];
        // Same packed-state skip as the irradiance sampler above.
        if (DDGI_ProbeSkipSampling(pd.state)) continue;

        const float3 tri = lerp(1.0 - alpha, alpha, float3(off));
        const float w = max(tri.x * tri.y * tri.z, 1e-3);
        sumSky += saturate(pd.skyVisibility) * w;
        sumW   += w;
    }

    if (sumW <= 1e-4) return float2(1.0, 0.0);
    return float2(saturate(sumSky / sumW), volumeWeight);
}

// ==== Directional sky visibility ====
// Each probe keeps the L1 moments of "this direction reaches the sky" from the
// same ray scan (DDGIProbeRelocate.cs): v0 = skyVisibility (the miss fraction)
// and v1 = skyVisDir (the mean of miss ? rayDir : 0). A shading point gets them
// from DDGI_SampleVolumeSkyMoments - interpolated with the irradiance's own
// visibility weights - and a direction reads them as
//     V(d) ~ v0 + 3 k dot(v1, d), clamped to [0, 1],
// k being the band-1 attenuation of the lobe it is read through: 1 for a
// direction, 2/3 for a cosine lobe - the cosine-weighted hemisphere visibility
// about N is v0 + 2 dot(v1, N). An open sky above (upper half missing) gives
// v0 = 0.5, v1 = (0, 0.25, 0): a floor sees 1, a wall 0.5, a ceiling 0 - which
// the scalar skyVisibility (0.5 for all three) cannot say.

// How much the probes' sky visibility is trusted at a point whose volume fade
// weight (the irradiance's coverage) is `fade`. Not the fade itself: the fade band
// hands the IRRADIANCE over to the sky fallback near the AABB, but the band is
// still inside the AABB and its probes still know what of the sky is visible.
// Weighted by the fade, the visibility would be weakest exactly where the
// unoccluded fallback is strongest: a volume fitted to a closed room has its
// floor, walls and ceiling in the band, and the room comes out 3-5x the path
// tracer. Full strength a short way into the band (fade >= 1/8); 0 outside the
// AABB (no probes), where the sky stays unoccluded.
#define DDGI_SKY_VIS_COVERAGE_GAIN 8.0
float DDGI_SkyVisCoverage(float fade)
{
    return saturate(fade * DDGI_SKY_VIS_COVERAGE_GAIN);
}

// DDGI_CombineSkyVisibility's arbitration for the moments: the winner's inside
// its fade, blended across it by bestFade; `coverage` = DDGI_SkyVisCoverage of
// the pick's coverage.
float4 DDGI_CombineSkyMoments(float4 best, float bestCov, float4 second, float secondCov,
                              DDGIVolumePick pick, out float coverage)
{
    const bool bestOk   = (pick.bestSlot   >= 0) && (bestCov   > 1e-4);
    const bool secondOk = (pick.secondSlot >= 0) && (secondCov > 1e-4);
    coverage = (bestOk || secondOk) ? DDGI_SkyVisCoverage(pick.coverage) : 0.0;
    if (!bestOk && !secondOk) return float4(1.0, 0.0, 0.0, 0.0);
    if (!bestOk)              return second;
    if (!secondOk)            return best;
    return lerp(second, best, saturate(pick.bestFade));
}

// V(d) from the moments; lobeK = 1 for a direction, 2/3 for a cosine lobe.
float DDGI_SkyVisibility(float4 moments, float3 dir, float lobeK)
{
    return saturate(moments.x + 3.0 * lobeK * dot(moments.yzw, dir));
}

// The band-1 attenuation of a reflection lobe of perceptual roughness r: 1 at
// r = 0 (the reflected direction itself), 2/3 at r = 1 (about a cosine lobe).
float DDGI_SkyLobeK(float roughness)
{
    return 1.0 - roughness * roughness * (1.0 / 3.0);
}

// DDGI_FUSED_SKY_SAMPLE 0 is a diagnostic switch: the separate irradiance and
// sky-visibility samplers, for reference-image parity checks without changing
// scene inputs or probe update scheduling.
#ifndef DDGI_FUSED_SKY_SAMPLE
#define DDGI_FUSED_SKY_SAMPLE 1
#endif
float4 DDGI_SampleVolumeWithSky(float3 P, float3 N, float3 V, DDGIVolumeGPU vol,
    Texture2D<float4> irr, Texture2D<float2> dep, StructuredBuffer<DDGIProbeData> data,
    SamplerState samp, out float2 sky)
{
#if DDGI_FUSED_SKY_SAMPLE
    float4 unusedMoments;
    float4 s = DDGI_SampleVolumeRawExWithSky(P,N,V,vol,irr,dep,data,samp,false,true,sky,false,unusedMoments);
    s.rgb *= vol.diffuseTint * vol.diffuseScale;
    return s;
#else
    sky = DDGI_SampleSkyVisibility(P,N,V,vol,data);
    return DDGI_SampleVolume(P,N,V,vol,irr,dep,data,samp);
#endif
}

#endif // DDGI_SAMPLING_HLSLI
