// Temporal anti-aliasing resolve (Karis 2014 / Salvi 2016 hybrid).
//   1. Dilate velocity over the 3x3 by nearest depth: the foreground silhouette's
//      velocity wins over background bleed (more robust than longest-magnitude with a
//      fast background behind a slow foreground). Sky pixels get a synthetic camera
//      motion vector.
//   2. Reproject with the dilated velocity -> previous-frame UV.
//   3. One 3x3 pass: Karis luma-weighted AABB stats, the Blackman-Harris de-jittered
//      current sample and the cardinal taps of a 5-tap unsharp. Blackman-Harris, not
//      a tent: the tent's wide footprint low-passes every frame and softens static
//      images.
//   4. No history, or prevUV off screen -> spatial-AA fallback (3x3 box blur). A
//      1.5 px edge inset ramps history confidence so the Catmull-Rom edge clamp does
//      not ghost.
//   5. History at prevUV through a 9-tap Catmull-Rom; a Salvi 5-tap cross blur of the
//      history luma is the anti-flicker reference.
//   6. Variance-clip the history; gamma widens for specular / low-variance /
//      high-frequency-static neighbourhoods, x1.5 in tonemap-blend mode to offset the
//      tonemap-compressed sigma.
//   7. Karis tonemap -> blend -> inverse tonemap. Velocity-adaptive alpha plus the
//      anti-flicker, bright-peak, high-frequency and disocclusion rules (the order is
//      listed above the blend).
//   8. Output .a holds an exponentially blended luma, the next frame's histPrevLuma.
// Modules:
//   TAA_Common.hlsli        - TAACB + colour-space / Karis tonemap / clip helpers
//   TAA_Reproject.hlsli     - depth-aware DilateVelocity, ReprojectUV
//   TAA_History.hlsli       - SampleHistoryCatmullRom9Tap
//   TAA_Neighbourhood.hlsli - ComputeNeighbourhood (3x3 stats, de-jitter, sharpen taps)
// Shared compute root signature (space2, RHI::Slots::Cmp):
//   b0 - TAACB
//   t0 - current jittered HDR scene
//   t1 - depth (R32_FLOAT view of D32_FLOAT)
//   t2 - history (previous resolved frame)
//   t3 - GBuffer surface (roughness in .r, metallic in .g)
//   t4 - velocity (NDC units, R16G16_FLOAT)
//   t5 - stencil-plane view of the depth buffer (outline / particle-VFX bits)
//   t6 - previous-frame depth (TAA-owned ping-pong, R32_FLOAT)
//   t7 - previous-frame outline coverage (TAA-owned ping-pong, R8_UNORM)
//   u0 - output (current resolved frame, the next frame's history)
//   u2 - this frame's depth -> next frame's t6
//   u3 - this frame's outline coverage -> next frame's t7

#include "TAA_Common.hlsli"
#include "TAA_Reproject.hlsli"
#include "TAA_History.hlsli"
#include "TAA_Neighbourhood.hlsli"

Texture2D<float4>   gCurrHDR       : register(t0, space2);
Texture2D<float>    gDepth         : register(t1, space2);
Texture2D<float4>   gHistory       : register(t2, space2);
Texture2D<float4>   gGBuffer       : register(t3, space2);
Texture2D<float2>   gVelocity      : register(t4, space2);
// Stencil-plane view of the depth buffer (X32_TYPELESS_G8X24_UINT of the
// D32_FLOAT_S8X24_UINT GBuffer depth). .y is the stencil byte, tested against
// outlineStencilBit (OutlinePass's inverted-hull rim) and particleVfxBit. Read only
// when one of those bits is non-zero; the C++ side clears them when no SRV is bound.
Texture2D<uint2>    gOutlineStencil: register(t5, space2);
Texture2D<float>    gPrevDepth     : register(t6, space2);
// Outline coverage, previous frame (TAA-owned ping-pong, R8_UNORM). 1 where the
// inverted-hull rim covered the texel. Supplies the disocclusion signal the
// depth buffer cannot: the hull writes no depth, so a texel the rim VACATED
// looks unchanged to the depth-based detector while history still holds the
// dark rim. Guarded by hasOutlineHist.
Texture2D<float>    gPrevOutlineMask: register(t7, space2);
RWTexture2D<float4> gOutput        : register(u0, space2);
RWTexture2D<float>  gPrevDepthOut  : register(u2, space2);
RWTexture2D<float>  gOutlineMaskOut: register(u3, space2);

SamplerState        gLinear        : register(s0, space2);

[numthreads(8, 8, 1)]
void CSMain(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;

    int2   dim = int2(width, height);
    float2 res = float2(dim);
    float2 uv  = (float2(id) + 0.5) / res;

    // 3x3 neighbourhood, computed before every early-out: its unweighted mean is also
    // the spatial-AA fallback when history is unusable.
    NeighbourhoodStats nh = ComputeNeighbourhood(gCurrHDR, int2(id), dim,
                                                  jitterX, jitterY);
    float3 currRaw = nh.centerHDR;          // raw jittered centre tap (high-freq-static path)
    float3 currBox = nh.meanRGB_HDR_unweighted;  // 3x3 box-blurred HDR (spatial-AA fallback)

    // Depth and velocity first: gPrevDepthOut is written before the no-history and
    // off-screen early returns, so the first frame after a resize or teleport still
    // leaves valid previous depth (garbage there false-fires the next frame's
    // disocclusion detector).
    float  currDepthVal = gDepth.Load(int3(id, 0)).r;
    float2 velocity     = DilateVelocity(gVelocity, gDepth, int2(id), dim);

    // This pixel's outline stencil tag, shared by the coverage record, the debug view
    // and the vacated-band test. Skipped when no stencil SRV is bound
    // (outlineStencilBit = 0).
    bool isOutlineRim = false;
    if (outlineStencilBit != 0)
        isOutlineRim = (gOutlineStencil.Load(int3(id, 0)).y & outlineStencilBit) != 0;

    // Record this frame's coverage for the NEXT frame's vacated-band test.
    // Written before any early-return so the history never develops holes (a
    // stale texel would read as "was outline" forever and permanently reject
    // history there).
    if (hasOutlineHist != 0)
        gOutlineMaskOut[id] = isOutlineRim ? 1.0 : 0.0;

    // Diagnostic: magenta = this pixel carries outlineStencilBit. Every outline path
    // below is gated on that bit, so no magenta ring around an outlined object means
    // they are all inert and the artefact comes through a path TAA cannot identify
    // (e.g. an outline drawn by the material's own shader, not by OutlinePass).
    if (outlineStencilDebug != 0)
    {
        float3 dbg = currRaw;
        if (isOutlineRim)
            dbg = float3(4.0, 0.0, 4.0);   // HDR magenta, unmistakable after tonemapping
        gOutput[id] = float4(dbg, Luminance(dbg));
        return;
    }

    // Synthetic sky / cloud motion vector. Sky (reversed-Z depth == 0) has no GBuffer
    // velocity: the skybox PS writes only colour and the volumetric-cloud composite
    // (before TAA) writes none. At zero velocity the clouds' history comes from the
    // same screen pixel while the freshly marched clouds move with the camera, so they
    // lag behind camera motion (clearly at 60 fps, sub-pixel at 180 fps).
    // Reconstruct the camera-reprojection velocity from a far-plane world point
    // (unjittered inverse VP -> world -> previous unjittered VP). At infinite distance
    // parallax is pure rotation, which covers nearly all of a distant cloud deck's
    // apparent motion; translation parallax at 1.5-4 km is sub-pixel. Unjittered
    // matrices keep the vector jitter-free, like the GBuffer's MS_Velocity.
    // Only when the dilated velocity is ~0: a sky pixel next to a moving foreground
    // silhouette keeps the neighbour's nearest-depth velocity (foreground-edge
    // anti-ghosting).
    if (currDepthVal <= 0.0 && dot(velocity, velocity) < 1e-12)
    {
        float2 curNDC = UVToNDC(uv);
        float4 farW   = mul(float4(curNDC, 0.0, 1.0), invViewProjNoJitter);
        float3 wpos   = farW.xyz / farW.w;
        float4 prevC  = mul(float4(wpos, 1.0), prevViewProj);
        velocity      = curNDC - prevC.xy / prevC.w;
    }

    gPrevDepthOut[id] = currDepthVal;

    // No history (first frame or resize) -> spatial-AA fallback: the 3x3 box mean, so
    // the first frame after a teleport / scene cut is not jagged.
    if (hasHistory < 0.5)
    {
        gOutput[id] = float4(currBox, Luminance(currBox));
        return;
    }

    // Specular detection. isSpecular: 0 = rough / diffuse, 1 = mirror-like. Widens the
    // AABB and raises the history weight, reducing specular jitter.
    float roughness  = gGBuffer.SampleLevel(gLinear, uv, 0).r;
    float isSpecular = 1.0 - smoothstep(0.0, specularRoughnessMax, roughness);

    // Reproject with the depth-dilated velocity.
    float2 prevUV = ReprojectUV(uv, velocity);

    // Disocclusion: prevUV off screen -> spatial-AA fallback. The 9-tap Catmull-Rom
    // reads +-1.5 px around prevUV; outside [0,1] history cannot recover.
    if (IsUVOutside(prevUV))
    {
        gOutput[id] = float4(currBox, Luminance(currBox));
        return;
    }

    // Edge confidence, a 1.5 px inset soft fade. Even with prevUV in range the
    // Catmull-Rom outer lobes sample up to 1.5 px outside the texture, and the CLAMP
    // sampler duplicates edge texels into spurious history (a thin ghost band along the
    // borders under camera motion). Ramp history confidence to 0 near the edge so
    // alpha moves toward 1 (full new sample).
    float2 edgePx     = min(prevUV, 1.0 - prevUV) * res;
    float  edgeConf   = saturate(min(edgePx.x, edgePx.y) - 0.5);  // 0 within 0.5 px, 1 from 1.5 px

    float3 m1    = nh.m1;
    float3 sigma = nh.sigma;
    float3 curr  = nh.currFilt;   // Blackman-Harris de-jittered current (Karis 2014)

    // velPx: motion in pixels per frame (NDC velocity x 0.5 x res). velFactor ramps
    // over [motionVelocityStartPx, motionVelocityEndPx]; geometry velocity is
    // jitter-free, so a low start is safe.
    float  velPx     = length(velocity * res * 0.5);
    float  velFactor = smoothstep(motionVelocityStartPx,
                                  max(motionVelocityEndPx,
                                      motionVelocityStartPx + 1e-3),
                                  velPx);

    // Noise-adaptive AABB gamma. isSpecular (from roughness) catches smooth specular,
    // but rough specular (roughness 0.3-0.5, isSpecular ~0.1-0.35) is just as noisy:
    // the neighbourhood Y variance is a second indicator.
    // Gated by (1 - velFactor): motion raises sigma through sub-pixel sample
    // variation, not specular noise, and an ungated isNoisySpecular would let
    // sharpKill drop the sharpen and blur moving content.
    float isNoisySpecular = smoothstep(0.02, 0.2, sigma.x) * (1.0 - velFactor);
    float isSpecularFull  = saturate(isSpecular + isNoisySpecular * (1.0 - isSpecular));

    // Tonemap-blend rescale: sigma is collected in tonemapped YCoCg, which compresses
    // highlights and shrinks sigma vs HDR linear. The CB gammas (defaults 1.5 / 2.0)
    // are HDR-linear so the editor sliders mean the same in both modes; the x1.5
    // compensation is applied here.
#if TAA_USE_TONEMAP_BLEND
    const float kTonemapSigmaScale = 1.5;
#else
    const float kTonemapSigmaScale = 1.0;
#endif
    float effectiveGamma  = lerp(colorBoxSigma, colorBoxSigmaSpecular, isSpecularFull) * kTonemapSigmaScale;

    // Widen the AABB for very-low-variance neighbourhoods (sky, distant geometry): a
    // tiny box rejects valid history on sub-pixel jitter (distant shimmer).
    float isLowVariance = 1.0 - smoothstep(0.01, 0.05, sigma.x);
    effectiveGamma      = lerp(effectiveGamma, effectiveGamma * 1.5, isLowVariance);

    // Mirror anti-ghost: a mirror's reflected image moves with virtual-depth parallax,
    // not the surface velocity TAA reprojects with, so under camera motion mirror
    // history is wrong and the widened specular / low-variance box only slows its
    // correction (a ghost trail of the reflection). Under real motion collapse the box
    // to the diffuse gamma; the reflection is already denoised upstream (SSRTemporal /
    // REBLUR). Velocity is jitter-free, so static mirrors keep the wide box.
    float isMirrorTAA  = 1.0 - smoothstep(0.0, 0.2, roughness);
    float mirrorMotion = isMirrorTAA * velFactor;
    effectiveGamma     = lerp(effectiveGamma,
                              colorBoxSigma * kTonemapSigmaScale, mirrorMotion);

    // High-frequency-static detector for fences / wires / foliage / lamp boundaries.
    // Symmetric bimodal test: the centre tap is close to either extreme of the
    // unweighted yMin / yMax (the Karis-weighted m1 is biased dim, so |centre - m1| /
    // sigma misses dark-centre patterns).
    //   binary {A,B} 50/50 -> bimodalRef = 0, bimodal = 1 (either centre)
    //   linear gradient {-1,0,+1}, centre 0 -> bimodalRef ~ sigma, bimodal = 0
    float bimodalRef  = min(abs(nh.centerY - nh.yMin),
                             abs(nh.centerY - nh.yMax));
    float bimodalDist = bimodalRef / max(nh.sigma.x, 1e-3);
    float bimodal     = 1.0 - smoothstep(0.4, 1.0, bimodalDist);

    // Contrast gate in HDR-linear space, not tonemapped sigma: c/(1+max) collapses an
    // HDR-bright bimodal (lamp 5 vs 100 -> 0.83 vs 0.99, sigma ~0.08) below the sigma
    // gate's mid-band. (max-min)/mid in HDR linear stays large for HDR-bright sources
    // and matches the LDR sigma gate on LDR fences.
    float hdrRange     = nh.lumaMaxHDR - nh.lumaMinHDR;
    float hdrMid       = max(0.5 * (nh.lumaMaxHDR + nh.lumaMinHDR), 1e-3);
    float hdrContrast  = hdrRange / hdrMid;
    float contrastGate = smoothstep(0.2, 0.6, hdrContrast);

    // HDR-bright bypass for the bimodal centre check: on HDR sources (lamps,
    // fireworks, glints) jitter often lands the centre tap on a partial-coverage value
    // between the two modes, which the check misses. HDR contrast alone is signal
    // enough; smooth LDR gradients never reach HDR brightness.
    float isHDRBright = smoothstep(2.0, 8.0, nh.lumaMaxHDR);
    float bimodalEffective = lerp(bimodal, 1.0, isHDRBright);

    // High-freq-static pixels get gamma 4; the detector is off under real motion
    // (1 - velFactor). Slow-motion disocclusion does not depend on this gate: the
    // disocclusion boost below raises alpha by force.
    float isHighFreqStatic = contrastGate
                           * bimodalEffective
                           * (1.0 - velFactor);
    effectiveGamma         = lerp(effectiveGamma, 4.0, isHighFreqStatic);

    // Karis-dimming protection. Under camera motion the 3x3 rarely contains the
    // brightest sub-pixel features (the jittered peak lands on neighbours) and the
    // Karis weight 1/(1+L) deweights the few that do, so the AABB clips reprojected
    // bright history down to yMax every frame: highlights and leaf sparkle darken
    // during motion. Widen the box with motion, up to (1 + velocityWiden)x; static
    // pixels keep tight clamping. Slightly more motion ghost beats luminance pulsing.
    // Not on high-freq-static pixels: gamma is already 4 there, and widening on top
    // makes a >5 sigma box that defeats the clip.
    // Density-aware:
    // - Dense LDR neighbourhoods (tile / foliage / brick) in motion: Catmull-Rom pulls
    //   in the foreground's previous colour, which fits inside any widened box (motion
    //   ghost). No widen there, and gamma shrinks by up to 50% to catch it.
    // - HDR-bright neighbourhoods (lamps / stars / glints): full widen, so
    //   motion-displaced bright peaks stay inside the box and bright history is not
    //   crushed.
    float neighbourhoodDensity = smoothstep(0.05, 0.15, sigma.x);
    float ldrDenseMotion       = neighbourhoodDensity * velFactor * (1.0 - isHDRBright);
    float effectiveWiden       = velocityWiden * (1.0 - neighbourhoodDensity * (1.0 - isHDRBright));
    effectiveGamma *= 1.0 + velFactor * effectiveWiden * (1.0 - isHighFreqStatic);
    effectiveGamma *= 1.0 - ldrDenseMotion * 0.5;

    // Karis 5-tap unsharp mask on the de-jittered curr:
    //   sharp = curr*5 - (top + bottom + left + right)
    // restores one pixel of high-frequency contrast that accumulated history loses,
    // without shifting uniform regions (curr*5 - curr*4 = curr).
    // Applied before the high-freq-static raw-centre substitution, so fence pixels
    // never see it.
    // Bright-peak gate: a centre already 3-5x its cardinal mean (specular peak, leaf
    // sparkle, sun glint) would be amplified ~5x more, enter history and accumulate
    // into bloom; peakRatio ramps the sharpen off over (2x, 5x).
    // Specular gate: on specular surfaces the centre-vs-cardinal difference is the
    // jitter-induced BRDF peak shift, so sharpening amplifies exactly the flicker TAA
    // integrates out; their detail comes from the BRDF lobe, so nothing is lost.
    if (sharpenStrength > 0.0)
    {
        float3 sharp           = curr * 5.0 - nh.cardinalSum;
        float3 cardinalMean    = nh.cardinalSum * 0.25;
        float  peakRatio       = Luminance(curr) / max(Luminance(cardinalMean), 1e-3);
        float  brightPeakKill  = smoothstep(2.0, 5.0, peakRatio);
        // Motion gate: drop the sharpen only on fast motion (> 2 px per frame: running,
        // camera flicks), where the 5x-amplified centre-tap swing would shimmer; slower
        // motion keeps full sharpen. Blackman-Harris does not pre-spread the centre tap
        // across the 3x3, so the delta the sharpen amplifies is signal, not jitter noise.
        float  sharpVelKill    = smoothstep(2.0, 8.0, velPx);
        float  sharpKill       = saturate(isHighFreqStatic + brightPeakKill + isSpecularFull + sharpVelKill);
        float  effStrength     = sharpenStrength * (1.0 - sharpKill);
        curr = lerp(curr, sharp, effStrength);
    }

    // For LDR fence-style sub-pixel geometry, replace the reconstructed curr with the
    // raw centre tap: Blackman-Harris still folds in some cardinal / corner weight
    // (~0.1 + ~0.01), which would collapse the jitter-phase coverage signal TAA needs
    // for 1-px-wide features. HDR-bright pixels skip this: their raw centre oscillates
    // bright / dark per jitter phase, and the reconstruction is steadier for
    // multi-pixel content.
    curr = lerp(curr, currRaw, isHighFreqStatic * (1.0 - isHDRBright));

    // Moving geometry should not spatially low-pass every current sample.
    // History still integrates the jitter sequence, while distant animated
    // silhouettes retain substantially more instantaneous detail.
    curr = lerp(curr, currRaw, saturate(velFactor * motionRawBlend));


    // HDR neighbourhood mean for the firefly clamp and the bright-peak detection below.
    // The unweighted 1/9 mean, not the Karis-weighted m1: that one is dragged down by
    // the 8 dimmer samples, so a legitimate sub-pixel HDR spike would be clamped to a
    // multiple of a too-small mean and leaf sparkle crushed regardless of the AABB.
    float3 meanRGB_HDR = nh.meanRGB_HDR_unweighted;
    float  meanLumaHDR = max(Luminance(meanRGB_HDR), 1e-3);

    // HDR soft clamp (UE4-style input pre-expansion). Foliage sparkle is a sub-pixel
    // HDR spike, up to ~50x its neighbourhood (a sunlit leaf edge caught through
    // jitter); the Karis-weighted AABB barely sees it, clips it, and history never
    // integrates it, so it flickers through every anti-flicker rule. Clamping curr to a
    // multiple of the neighbourhood mean before the clip keeps extreme peaks out of
    // history.
    // The ratio follows the mean luma: 4x when very dim (indoor firefly noise barely
    // above ambient) to 16x at sunlight scale (legitimate glints exceed 10x), through a
    // smoothstep so a bright / dim seam such as a window edge does not jump.
    {
        float currLumaHDR = Luminance(curr);
        float kFireflyRatio = lerp(4.0, 16.0, smoothstep(0.1, 5.0, meanLumaHDR));
        float maxAllowed = meanLumaHDR * kFireflyRatio;
        if (currLumaHDR > maxAllowed)
            curr *= maxAllowed / currLumaHDR;
    }

    float3 nMin = m1 - effectiveGamma * sigma;
    float3 nMax = m1 + effectiveGamma * sigma;

    // Motion-time AABB Y expansion, HDR-bright neighbourhoods only: the Karis-weighted
    // m1 / sigma undercount bright pixels, so HDR stars and specular peaks fall outside
    // the box during motion; expanding to the unweighted yMin / yMax keeps them. In LDR
    // it would only let Catmull-Rom contamination through (motion ghost on dense
    // backgrounds such as tile floors).
    float aabbExpand = velFactor * isHDRBright;
    if (aabbExpand > 0.0)
    {
        nMin.x = lerp(nMin.x, min(nMin.x, nh.yMin), aabbExpand);
        nMax.x = lerp(nMax.x, max(nMax.x, nh.yMax), aabbExpand);
    }

    // History: 9-tap Catmull-Rom + variance clip. Clamp to >= 0: negative-lobe
    // results flip sign after ToneMapLuma and amplify into fireflies.
    // Cross-fade Catmull-Rom -> bilinear with fast motion: the 9 taps span 4x4 pixels,
    // so at sub-pixel offsets they pick up colour from texels a thin moving feature
    // (column, wire, pole) covered last frame, and in dense neighbourhoods that
    // contamination survives the clip as a ghost trail. Bilinear's 2x2 footprint stays
    // near prevUV; motion blur hides its softness.
    float3 histLinearCR    = SampleHistoryCatmullRom9Tap(gHistory, gLinear,
                                                          prevUV * res, res);
    float3 histLinearBilin = gHistory.SampleLevel(gLinear, prevUV, 0).rgb;
    // Only genuinely fast swings (camera flicks, 8-20 px per frame) fall back to
    // bilinear: walking / panning stays under 8 px, and bilinear's 2x2 footprint
    // visibly softens textures; Catmull-Rom's max(0, ...) clamp handles its own
    // contamination.
    float  useBilinear     = smoothstep(8.0, 20.0, velPx);

    float3 histLinear      = max(lerp(histLinearCR, histLinearBilin, useBilinear), 0.0);
    float  histPrevLuma = gHistory.SampleLevel(gLinear, prevUV, 0).a;  // blended luma the previous frame stored in .a

    // Salvi 5-tap cross blur of the history luma: the lumaGain anti-flicker reference.
    // Luminance(hist) alone misses 1-px specular flicker (leaves, water, wet metal)
    // because hist holds the same frequency; a spatially smoothed reference decouples
    // detection from the flickering signal. HDR-linear taps keep the comparison space
    // independent of TAA_USE_TONEMAP_BLEND.
    float lumaHistSmooth;
    {
        float2 uvStep = 1.0 / res;
        float3 hN = gHistory.SampleLevel(gLinear, prevUV + float2(0.0,         -uvStep.y), 0).rgb;
        float3 hS = gHistory.SampleLevel(gLinear, prevUV + float2(0.0,          uvStep.y), 0).rgb;
        float3 hE = gHistory.SampleLevel(gLinear, prevUV + float2( uvStep.x,    0.0     ), 0).rgb;
        float3 hW = gHistory.SampleLevel(gLinear, prevUV + float2(-uvStep.x,    0.0     ), 0).rgb;
        lumaHistSmooth = 0.2 * (Luminance(histLinear) + Luminance(hN) + Luminance(hS) + Luminance(hE) + Luminance(hW));
    }

#if TAA_USE_TONEMAP_BLEND
    float3 hist = ToneMapLuma(histLinear);
#else
    float3 hist = histLinear;
#endif
    float3 histYCoCg = RGBToYCoCg(hist);
    float  histY     = histYCoCg.x;   // pre-clip Y, saved for distance-to-clamp below

    // Fast-swing HDR clip bypass: under a fast swing the current 3x3 can miss
    // sub-pixel bright features entirely. Keep the unclipped history when its Y exceeds
    // the unweighted max, the motion is fast enough that reprojection beats the
    // disocclusion risk, and the neighbourhood is HDR-bright (isHDRBright keeps it off
    // in LDR, where a character revealing a wall passes the histY > yMax test too).
    float3 histClipped = YCoCgToRGB(ClipAABB(nMin, nMax, histYCoCg));
    float3 histRaw     = YCoCgToRGB(histYCoCg);
    float  bypassClip  = smoothstep(1.0, 5.0, velPx)
                       * smoothstep(0.0, 0.2, histY - nh.yMax)
                       * isHDRBright;
    hist = lerp(histClipped, histRaw, bypassClip);

    // Disocclusion signal from the clip magnitude: if the variance clip moved history
    // a lot, the content changed (foreground passed, occluder revealed). Used below as
    // an upward alpha boost: pushing alpha to 0.5 lets curr dominate so wrong history
    // decays in 1-2 frames, where only lifting the damps would keep the low baseline
    // alpha and let polluted history in.
    float3 clipDelta = histRaw - histClipped;
    float  clipMag   = max(max(abs(clipDelta.r), abs(clipDelta.g)), abs(clipDelta.b));

    // Velocity-adaptive blend. isSpecularFull drives both the base alpha and
    // lumaGain, so rough noisy specular gets the same long-history treatment as smooth
    // specular.
    // Do not pull the blend target toward the 3x3 mean on high-freq-static pixels:
    // around one fence slat that mean is ~background (1/9 slat), so the fence would
    // fade instead of integrating. Each frame's jitter-phase coverage must be kept and
    // integrated across frames.
#if TAA_USE_TONEMAP_BLEND
    float3 currT = ToneMapLuma(curr);
#else
    float3 currT = curr;   // linear-blend mode: not tonemapped despite the name
#endif
    // Alpha composition, in order:
    //   (1)  baseAlpha = lerp(diffuse, specular, isSpecularFull)
    //   (2)  lerp(a, max(a, 0.15), velFactor)                    velocity boost
    //   (3)  + lumaGain x isSpecularFull x (maxFromLuma - a)     anti-flicker, bidirectional
    //   (4)  x distToClamp / (distToClamp + boxW)                distance-to-clamp (antiFlicker)
    //   (5)  lerp(a, min(a, 0.1), flickerDamp)                   bright-peak damp
    //   (6)  lerp(a, min(a, 0.03), isHighFreqStatic)             fence / wire damp
    //   (6b) lerp(a, min(a, 0.01), distantSpecDamp)              distant specular crush
    //   (7)  lerp(a, min(a, 0.02), mirrorDamp)                   mirror-shimmer damp
    //   (8)  max(a, 0.25 x mirrorMotion), max(a, motionAlphaFloor x velFactor)   motion floors
    //   (9)  disocclusion boosts (0.5 LDR depth / clip, 0.4 HDR depth)
    //   (10) outline / vacated-band / particle-VFX floors
    //   (11) lerp(1, a, edgeConf)                                edge fade, last
    // The damps (5)-(7) set ceilings; the floors and boosts (8)-(10) come after them
    // and win; the edge fade overrides everything within 1.5 px of the border, where
    // Catmull-Rom samples clamped edge texels.
    // Direct history weight (TAACB.historyWeight): specular alpha = diffuse x 0.125, a
    // longer specular history without a second slider.
    float  diffuseAlpha  = 1.0 - saturate(historyWeight);
    float  specularAlpha = diffuseAlpha * 0.125;
    float  baseAlpha     = lerp(diffuseAlpha, specularAlpha, isSpecularFull);
    // Velocity boost to 0.15. A higher target (0.5) acts like a 2-frame box filter
    // under sustained camera motion and lets each jitter phase's HDR sub-pixel alias
    // into history (shimmer). Real disocclusion still boosts to 0.5 below.
    float  alpha         = lerp(baseAlpha, max(baseAlpha, 0.15), velFactor);

    // Luminance-gain anti-flicker (specular / noisy regions). |dLuma| is
    // bidirectional, so a highlight leaving the pixel (bright -> dark) triggers it as
    // well as one arriving.
    // lumaH is the Salvi-smoothed history luma mixed with 30% of the temporal luma from
    // history.a: the spatial blur catches peaks shifting between adjacent pixels, the
    // temporal term a chrome peak blinking at one pixel. A weight above ~0.3 lets a
    // single-frame outlier in the temporal channel dominate. lumaC is HDR linear like
    // lumaH, so both sides share a space.
    float lumaH    = lerp(lumaHistSmooth, histPrevLuma, 0.3);
    float lumaC    = Luminance(curr);
    float lumaGain = saturate(abs(lumaC - lumaH) / max(lumaC + lumaH + 0.01, 0.01));
    float maxAlphaFromLuma = lerp(0.05, 0.1, isSpecularFull);
    alpha = saturate(alpha + lumaGain * isSpecularFull * (maxAlphaFromLuma - alpha));

    // Distance-to-clamp anti-flicker (Falcor; Karis 2014 / Salvi 2016): when the
    // pre-clip history Y sits near or beyond the AABB Y boundary the clamp is doing
    // real work, and taking the full new sample would flicker. Scale alpha by
    //   d / (d + boxWidth)
    // so the blend leans toward history when the clamp squeezes it and stays near the
    // baseline when history is well inside. Luminance only; chroma flicker is rarely
    // visible. This lowers alpha multiplicatively; the bright-peak / high-freq damps
    // below still take precedence.
    if (antiFlicker != 0)
    {
        float yMin = m1.x - effectiveGamma * sigma.x;
        float yMax = m1.x + effectiveGamma * sigma.x;
        float distToClamp = min(abs(yMin - histY), abs(yMax - histY));
        float boxW = max(yMax - yMin, 1e-5);
        float reduced = saturate(alpha * distToClamp / (distToClamp + boxW));
        // Mirror anti-ghost: on a moving mirror, history at the box edge means the
        // reflection moved (wrong surface-velocity reprojection), not jitter flicker, and
        // reducing alpha would lock the ghost in. Keep the reduction only when near-static
        // (mirrorMotion -> 0).
        alpha = lerp(reduced, alpha, mirrorMotion);
    }

    // Bright-peak detection in HDR space: ToneMapLuma compresses a 50x HDR peak to
    // ~2x, too weak to detect. Leaf / sun peaks are 5-20x in HDR; the (2, 8) range
    // covers that without firing on normal contrast. curr is already firefly-clamped
    // above, so this catches the peaks the clamp let through (e.g. 6x under an 8x
    // ceiling).
    float currLumaHDR  = Luminance(curr);
    float isBrightPeak = smoothstep(2.0, 8.0, currLumaHDR / meanLumaHDR);


    // Bright-peak anti-flicker: for static bright peaks push alpha down so the
    // per-frame jittered luminance averages out in history; moving bright features
    // keep tracking via their velocity. nearStatic is looser than velFactor: leaves in
    // wind, water and grass sway 0.5-2 px, yet their specular spikes still need
    // damping.
    float nearStatic  = 1.0 - smoothstep(0.5, 3.0, velPx);
    float flickerDamp = isBrightPeak * nearStatic;
    alpha = lerp(alpha, min(alpha, 0.1), flickerDamp);

    // High-freq-static alpha crush: 0.03 = 3% new information per frame, a ~33-frame
    // convergence window.
    alpha = lerp(alpha, min(alpha, 0.03), isHighFreqStatic);

    // Distant specular extra crush: high-freq-static + specular means a sub-pixel
    // peak shifting between adjacent pixels at 20-50x peak / dim. Alpha 0.03 still
    // leaves a visible per-frame swing; 0.01 (~100-frame window) drops it below
    // visibility.
    float distantSpecDamp = isHighFreqStatic * isSpecularFull;
    alpha = lerp(alpha, min(alpha, 0.01), distantSpecDamp);

    // Mirror-shimmer alpha crush (chrome / wet / very low roughness). Below roughness
    // ~0.1 the sub-pixel specular peak shifts between pixels with each jitter phase;
    // the specular base alpha (~0.025 at the default historyWeight) plus lumaGain
    // pulling toward 0.1 leaves a visible per-frame swing. Crush alpha to 0.02
    // (~50-frame window) when near-static. The cost is a slow response to real mirror
    // content changes; camera motion turns it off (nearStatic -> 0 by 3 px).
    float isMirror     = 1.0 - smoothstep(0.0, 0.2, roughness);
    // Release on clipMag: when the clip moved history a lot the content changed
    // (occluder swept past, background revealed), the damp's premise is void, and
    // holding alpha at 0.02 smears stale history into a long trail on mirror-like
    // surfaces (water). Jitter-phase sparkle moves history far less than the 0.05-0.2
    // clip band, so shimmer integration is unaffected.
    float mirrorDamp   = isMirror * (1.0 - smoothstep(0.5, 3.0, velPx))
                       * (1.0 - smoothstep(0.05, 0.2, clipMag));
    alpha = lerp(alpha, min(alpha, 0.02), mirrorDamp);

    // Mirror anti-ghost alpha floor: under real motion a mirror's history is
    // mis-reprojected (see the box collapse above), so guarantee enough new-sample
    // inflow that the wrong history dies in a few frames. Overrides the specular /
    // luma caps; static mirrors (mirrorMotion = 0) stay on the shimmer damp.
    alpha = max(alpha, 0.25 * mirrorMotion);

    // Restore responsiveness after all static/specular anti-flicker caps.
    // Without this, distant moving high-frequency geometry can be forced to
    // alpha 0.01-0.03 (33-100 frames of history) despite valid velocity.
    alpha = max(alpha, saturate(motionAlphaFloor) * velFactor);

    // Disocclusion alpha boost, the max of a depth and a clip-magnitude detector:
    //   depthMM - previous depth point-sampled at prevUV. Catches a thin foreground over
    //     a dense background, where the box is wide enough that Catmull-Rom
    //     contamination fits inside (clipMag stays 0).
    //   clipMag - coplanar disocclusion (foreground and background at similar depth),
    //     where depth fails.
    // Both are off on HDR-bright pixels, which rely on the high-freq-static / mirror /
    // bright-peak damps: sub-pixel jitter at HDR specular edges trips depthMM and would
    // bypass them (edge flicker).
    // Boost target 0.5: ~3-frame (~50 ms) convergence, mild enough that false fires do
    // not flash.
    {
        int2  prevPxI    = clamp(int2(prevUV * res), int2(0, 0), dim - 1);
        float prevDepthV = gPrevDepth.Load(int3(prevPxI, 0)).r;
        float depthMM    = abs(currDepthVal - prevDepthV) / max(currDepthVal, 1e-5);

        float ldrGate     = 1.0 - isHDRBright;
        float disoccDepth = smoothstep(0.04, 0.12, depthMM) * ldrGate;
        float disoccClip  = smoothstep(0.05, 0.2,  clipMag) * ldrGate;
        float disocclusion = max(disoccDepth, disoccClip);

        alpha = lerp(alpha, max(alpha, 0.5), disocclusion);

        // HDR-bright pixels get a tighter, weaker depth-only rescue. Without one, on water
        // (specular alpha ~0.025, mirror damp 0.02, gamma ~3) a revealed background pixel
        // would keep its occluder's history for 40+ frames. A 15-35% relative depth gate
        // still fires on real foreground / background disocclusion (the gap is ~100%)
        // while jitter noise along a continuous surface stays under it; target 0.4 keeps
        // residual edge flicker mild.
        float disoccDepthHDR = smoothstep(0.15, 0.35, depthMM) * isHDRBright;
        alpha = lerp(alpha, max(alpha, 0.4), disoccDepthHDR);
    }

    // Outline-aware history weakening. OutlinePass stamps a stencil bit on every rim
    // pixel of its inverted-hull sub-pass. The rim shares the background's velocity and
    // depth (the hull writes colour, not depth), so TAA reprojects it as stationary
    // background and leaves a trail behind a moving silhouette. outlineMinAlpha floors
    // alpha on rim pixels (default 0: the vacated-band test below handles the trail).
    // Gated by non-zero bits; the C++ side clears outlineStencilBit when no stencil SRV
    // is bound. After every damp so none can undo it, before the edge fade, which
    // border pixels need whatever their outline state.
    if (outlineStencilBit != 0 || particleVfxBit != 0)
    {
        uint stencilByte = gOutlineStencil.Load(int3(id, 0)).y;
        if (outlineStencilBit != 0 && (stencilByte & outlineStencilBit) != 0)
            alpha = max(alpha, outlineMinAlpha);

        // Vacated band: this texel is not rim now, but the rim covered the texel our
        // history came from (the outline-shaped trail where the character passed). Every
        // other rejection path misses it:
        //   * depth disocclusion - the hull writes no depth, so the texel's depth is
        //     background before and after. (The character's body does write depth, which
        //     is why only the outline leaves a trail.)
        //   * velocity - the texel is background now, so it reprojects onto itself and
        //     pulls the stale rim straight out of history.
        //   * the 3x3 variance clamp - the rim's new position is a texel or two away, so
        //     the box already contains the outline colour and the stale value sits inside.
        // Sampled at prevUV (filtered, so the band is slightly dilated): the texel history
        // actually came from, not this one.
        if (hasOutlineHist != 0 && outlineVacatedAlpha > 0.0 && !isOutlineRim)
        {
            float wasRim = gPrevOutlineMask.SampleLevel(gLinear, prevUV, 0).r;
            alpha = lerp(alpha, max(alpha, outlineVacatedAlpha), saturate(wasRim));
        }
        // Particle VFX (additive / unlit sparks etc.) stamp a stencil bit. Like outlines
        // they write colour but no velocity / depth, so TAA would ghost them as background:
        // floor alpha near 1 (particleVfxMinAlpha) so they stay sharp. Per-emitter opt-in
        // (excludeFromTAA); soft / lit particles leave the bit unset and keep full TAA.
        if (particleVfxBit != 0 && (stencilByte & particleVfxBit) != 0)
            alpha = max(alpha, particleVfxMinAlpha);
    }

    // Edge fade: within 1.5 px of a border edgeConf < 1 fades alpha toward 1 (no
    // history). The last modifier, overriding every rule: Catmull-Rom samples clamped
    // edge texels there.
    alpha = lerp(1.0, alpha, edgeConf);

    float3 resolved = lerp(hist, currT, alpha);

    // Fast-swing HDR max-protect with decay: keeps sub-pixel HDR features (stars,
    // distant specular peaks) from vanishing in rapid camera swings, where Catmull-Rom
    // sub-pixel sampling and missed frames dim them cumulatively.
    // max(resolved, hist x 0.88) holds history against dim-curr washout with a bounded
    // fade, so moving past a bright source leaves no permanent ghost: with Catmull-Rom
    // (~0.7) and the dim-curr blend the decay is ~0.6 per frame, gone in ~6-10 frames
    // (~150 ms at 60 fps). Only fast swings (8-20 px per frame) engage it; moderate
    // motion relies on the AABB Y expansion and the clip bypass, which do not ghost.
    float fastMotionHDR = isHDRBright * smoothstep(8.0, 20.0, velPx);
    resolved = lerp(resolved, max(resolved, hist * 0.88), fastMotionHDR);

    // Output .a: an exponentially blended luma, read back next frame as histPrevLuma
    // (the temporal anti-flicker reference). The blend rate is max(alpha, 0.05): 5% per
    // frame with good history, tracking the current frame when alpha is high (motion,
    // disocclusion), so it does not lag through teleports.
    float resolvedLumaHDR;
#if TAA_USE_TONEMAP_BLEND
    // Saturation guard: max-channel ToneMapLuma saturates at max(channel) = 1 and
    // InvToneMapLuma amplifies by 1 / (1 - max). After the clip and lerp on a wide AABB
    // (the x1.5 sigma rescale and the specular gamma stack) channels can land at
    // 0.98-0.999, i.e. 50-1000x HDR spikes that accumulate into history and trigger
    // bloom. Capping at 0.99 bounds the amplification at ~99x, well above the firefly
    // clamp's 16x ceiling, so legitimate highlights are unaffected.
    {
        float maxR = max(max(resolved.r, resolved.g), resolved.b);
        if (maxR > 0.99) resolved *= 0.99 / maxR;
    }
    float3 outRGB = max(InvToneMapLuma(resolved), 0.0);
#else
    float3 outRGB = max(resolved, 0.0);
#endif
    resolvedLumaHDR = Luminance(outRGB);
    float lumaBlend  = max(alpha, 0.05);
    float outLuma    = lerp(histPrevLuma, resolvedLumaHDR, lumaBlend);
    gOutput[id] = float4(outRGB, outLuma);
}
