// Procedural, photographic directional-light lens flare. Writes an additive
// RGBA16F contribution at half resolution; ToneMap.cs.hlsl samples it as
// g_lensFlare and adds it to the HDR scene before tonemapping.
//
// What keeps a procedural flare from reading as fake, most important first:
//  1. Ghosts are images of the aperture: polygons (one side per iris blade),
//     brightest at the rim. A centre-bright round gradient is the classic CG
//     tell. See ApertureGhost().
//  2. Occlusion is continuous: distance-weighted taps over two rings plus an
//     exponential moving average across frames in a persistent 1x1 buffer.
//     Binary taps step visibly and flicker as the sun crosses a branch or pole.
//  3. The sun glow is Lorentzian (tight core, long faint tail), not a smoothstep
//     disc, so it reads as light scattering in the lens.
//  4. Chromatic aberration is spectral: each ghost is sampled along a wavelength
//     ramp and integrated, a continuous rainbow smear instead of hard red/blue
//     fringes.
//  5. Ghost size, brightness and spacing vary, and some ghosts land on the far
//     side of the sun; an even string of beads looks fake.
//  6. The streak fades exponentially; a linear cutoff shows a straight edge.
//
// Deliberately not here: a lens-dirt texture. It is the most valuable single
// addition but needs an authored grayscale asset; its hook belongs next to g_cloud.
//
// Compute root signature (space2):
//   [0] cbuffer PerDispatch b0 space2
//   [1] SRV t0 space2 - hardware depth (reverse-Z; 0 = far/sky, 1 = near)
//   [2] SRV t1 space2 - cloud raymarch (a = view-ray transmittance)
//   [4] UAV u0 space2 - output flare RGBA16F (overwrite)
//   [5] UAV u1 space2 - 1x1 R32_FLOAT occlusion history (persistent, never cleared)

cbuffer PerDispatch : register(b0, space2)
{
    uint  g_dstWidth;
    uint  g_dstHeight;
    uint  g_srcDepthWidth;
    uint  g_srcDepthHeight;

    uint  g_enabled;          // 1 = generate flare; 0 = clear to black
    float g_intensity;        // global multiplier
    float g_sunBehind;        // 1 = sun behind camera; skip
    float g_chromaticOffset;  // UV-space spectral spread per ghost

    float2 g_sunUV;           // sun in screen-space UV (can be slightly outside 0..1)
    float  g_haloWidth;       // 0..0.5 (fraction of screen along Y)
    float  g_streakLength;    // 0..1, anamorphic horizontal streak length

    float3 g_sunColor;        // raw sun color (already brightness scaled)
    float  g_ghostDispersal;  // 0..1 (ghost spacing, fraction of sun -> center)

    uint   g_ghostCount;      // 0..8
    float  g_streakWidth;     // vertical thickness of streak
    float  g_occlusionRadius; // UV radius for depth taps
    uint   g_cloudValid;      // 1 = g_cloud bound and rendered this frame

    float  g_apertureBlades;  // iris blade count -> ghost polygon sides (0 = round)
    float  g_apertureRotation;// radians, so the polygon is not axis-aligned
    float  g_occlusionSmooth; // EMA factor per frame, 0 = instant, 0.9 = very smooth
    uint   g_historyValid;    // 0 on the first frame / after a resize -> seed the EMA

    float  g_streakStrength;  // anamorphic streak weight (photographic = low)
    float  g_ghostBrightness; // master on the ghost chain
    float  g_glareStrength;   // veiling glare (the long soft tail around the sun)
    float  g_pad0;
};

Texture2D<float>     g_depth   : register(t0, space2);
Texture2D<float4>    g_cloud   : register(t1, space2);
RWTexture2D<float4>  g_output  : register(u0, space2);
RWTexture2D<float>   g_occHist : register(u1, space2);   // 1x1, persistent
SamplerState         g_linear  : register(s0, space2);

static const float kTau = 6.2831853071795864;

// Veiling glare: a Lorentzian, not a smoothstep disc. Scattering in the lens
// stack and the atmosphere gives a 1/(1+r^2) profile, a tight bright core with a
// tail that never quite ends; a smoothstep disc has a hard support radius and reads
// as a painted circle.
float VeilingGlare(float2 uv, float2 center, float radius)
{
    const float d = distance(uv, center) / max(radius, 1e-5);
    return 1.0 / (1.0 + d * d * 4.0);
}

// Distance to a regular-polygon boundary (circumradius 1) in the direction of d,
// rotated by rotation, so length(d) / (ApertureEdge(...) x radius) is 1 exactly on
// the edge. blades < 3 is a circle (a perfectly round iris).
float ApertureEdge(float2 d, float blades, float rotation)
{
    if (blades < 2.5) return 1.0;
    const float seg = kTau / blades;
    float a = atan2(d.y, d.x) + rotation;
    // Fold the angle into one blade sector, centred on the sector midpoint.
    a = a - seg * floor(a / seg) - seg * 0.5;
    return cos(seg * 0.5) / max(cos(a), 1e-4);
}

// A defocused image of the aperture: a polygon that is brightest at its rim.
//   body - the filled polygon with a soft edge, dim: an out-of-focus aperture is
//          mostly a faint wash.
//   rim  - a bright band just inside the boundary; it makes the ghost read as an
//          optical artefact instead of a sticker.
float ApertureGhost(float2 uv, float2 center, float radius,
                    float blades, float rotation)
{
    const float2 d = uv - center;
    const float  r = length(d);
    const float  e = ApertureEdge(d, blades, rotation) * max(radius, 1e-5);
    const float  t = r / e;                       // 1.0 == on the boundary

    const float body = 1.0 - smoothstep(0.72, 1.02, t);
    const float x    = (t - 0.90) / 0.13;
    const float rim  = exp(-x * x);
    return body * 0.30 + rim * body * 0.85;
}

// Cheap spectral tint. t in [0,1] sweeps blue -> green -> red; the three lobes sum
// to roughly constant luminance, so integrating uniformly over t and dividing by
// the accumulated weight gives back white for zero dispersion.
float3 SpectrumTint(float t)
{
    const float c = 3.0 * t - 1.5;
    return saturate(float3(c, 1.0 - abs(c), -c));
}

// Sun occlusion: two rings of distance-weighted taps around a heavier centre tap.
// The depth test itself is binary (a texel is sky or not), so continuity comes
// from the weighting (an occluder entering the disc changes the sum gradually) and
// from the caller's temporal EMA. Reverse-Z: depth 0 is the far plane, so
// <= epsilon is sky.
float SampleSunOcclusion(float2 sunUV)
{
    if (any(sunUV < 0.0) || any(sunUV > 1.0))
        return 1.0;

    const float kThresh = 1e-4;
    float visible = 0.0;
    float total   = 0.0;

    // Centre tap carries the most weight: an occluder covering the sun itself
    // matters far more than one clipping the edge of the sampling disc.
    visible += (g_depth.SampleLevel(g_linear, sunUV, 0).r <= kThresh) ? 2.0 : 0.0;
    total   += 2.0;

    [unroll]
    for (int ring = 0; ring < 2; ++ring)
    {
        const float rr = g_occlusionRadius * (ring == 0 ? 0.5 : 1.0);
        const float w  = (ring == 0) ? 1.0 : 0.5;
        [unroll]
        for (int i = 0; i < 8; ++i)
        {
            const float ang = kTau * (float(i) + 0.5 * float(ring)) / 8.0;
            const float2 uv = saturate(sunUV + float2(cos(ang), sin(ang)) * rr);
            visible += (g_depth.SampleLevel(g_linear, uv, 0).r <= kThresh) ? w : 0.0;
            total   += w;
        }
    }
    return visible / total;
}

// Volumetric clouds never write depth, so the depth test sees clear sky through
// full overcast. Sample the raymarch's transmittance (alpha; 1 = clear) in the same
// disc: continuous by nature, so the flare fades smoothly as a cloud edge crosses
// the sun.
float SampleCloudTransmittance(float2 sunUV)
{
    if (g_cloudValid == 0u) return 1.0;
    if (any(sunUV < 0.0) || any(sunUV > 1.0)) return 1.0;

    float tr = g_cloud.SampleLevel(g_linear, sunUV, 0).a;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const float ang = kTau * (float(i) + 0.5) / 8.0;
        const float2 uv = saturate(sunUV + float2(cos(ang), sin(ang))
                                          * (g_occlusionRadius * 2.0));
        tr += g_cloud.SampleLevel(g_linear, uv, 0).a;
    }
    return tr / 9.0;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstWidth || id.y >= g_dstHeight) return;

    if (g_enabled == 0 || g_sunBehind > 0.5)
    {
        g_output[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        // Reset the history to visible so re-enabling does not pop from a stale
        // occluded value.
        if (id.x == 0 && id.y == 0) g_occHist[uint2(0, 0)] = 1.0;
        return;
    }

    const float2 uv = (float2(id.xy) + 0.5) / float2(g_dstWidth, g_dstHeight);

    // Aspect-correct UV so circles stay circular and ghost spacing is even.
    const float  aspect = float(g_dstWidth) / max(1.0, float(g_dstHeight));
    const float2 uvAR   = float2((uv.x      - 0.5) * aspect + 0.5, uv.y);
    const float2 sunAR  = float2((g_sunUV.x - 0.5) * aspect + 0.5, g_sunUV.y);
    const float2 ctrAR  = float2(0.5, 0.5);

    // Sun on-screen edge fade by the distance to the nearest screen border.
    const float2 e2       = min(g_sunUV, 1.0 - g_sunUV);
    const float  edgeDist = min(e2.x, e2.y);
    const float  onScreen = smoothstep(-0.05, 0.12, edgeDist);

    // ---- Occlusion, temporally filtered ----
    // Every thread computes the same value from the same inputs (the raw term depends
    // only on g_sunUV, and the history is last frame's, stable for the whole dispatch),
    // so there is no race and no barrier; one thread writes the result back.
    const float rawOcc  = SampleSunOcclusion(g_sunUV) * SampleCloudTransmittance(g_sunUV);
    const float prevOcc = (g_historyValid != 0u) ? g_occHist[uint2(0, 0)] : rawOcc;
    const float occ     = lerp(rawOcc, prevOcc, saturate(g_occlusionSmooth));
    if (id.x == 0 && id.y == 0) g_occHist[uint2(0, 0)] = occ;

    const float intensity = g_intensity * onScreen * occ;
    if (intensity <= 1e-4)
    {
        g_output[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float3 color = 0.0;

    // ---- Sun core + veiling glare ----
    // Two Lorentzians: a tight core and a wide faint scatter. The wide one sells
    // "bright light in a real lens"; keep its amplitude low or it turns into fog over
    // the whole frame.
    color += g_sunColor * VeilingGlare(uvAR, sunAR, 0.030) * 3.0;
    color += g_sunColor * VeilingGlare(uvAR, sunAR, 0.170) * (0.30 * g_glareStrength);

    // ---- Halo ring ----
    // The halo is itself an aperture image, so it gets the polygon shape and a
    // spectral sweep across its width: a faint rainbow across a big soft ring is a
    // strong realism cue and costs almost nothing.
    {
        const float2 d = uvAR - sunAR;
        const float  r = length(d);
        const float  e = ApertureEdge(d, g_apertureBlades, g_apertureRotation);
        const float  t = r / max(g_haloWidth * e, 1e-5);
        const float  x = (t - 1.0) / 0.45;
        const float  ring = exp(-x * x);
        // Spectral across the ring's radial profile.
        const float3 tint = SpectrumTint(saturate(0.5 + (t - 1.0) * 1.1));
        color += g_sunColor * tint * ring * 0.16;
    }

    // ---- Anamorphic horizontal streak ----
    // Exponential in both axes: a linear 1 - |dx|/len falloff ends the streak with a
    // visible straight edge.
    {
        const float dx = uvAR.x - sunAR.x;
        const float dy = uvAR.y - sunAR.y;
        const float body = exp(-(dy * dy) / max(1e-5, g_streakWidth * g_streakWidth));
        const float fade = exp(-abs(dx) / max(1e-4, g_streakLength * 0.35));
        // Tinted by the sun with only a slight cool bias: a hardcoded blue bar is the
        // anamorphic-lens look, not the photographic one.
        const float3 tint = g_sunColor * float3(0.85, 0.95, 1.15);
        color += tint * body * fade * g_streakStrength;
    }

    // ---- Ghost chain ----
    const float2 sunToCenter   = float2(0.5, 0.5) - g_sunUV;
    const float2 sunToCenterAR = float2(sunToCenter.x * aspect, sunToCenter.y);
    const float  dispLen       = length(sunToCenterAR) + 1e-5;
    const float2 ghostDir      = sunToCenterAR / dispLen;
    // Perpendicular, for the small lateral offset real ghost chains have: a perfectly
    // collinear chain is another giveaway.
    const float2 ghostPerp     = float2(-ghostDir.y, ghostDir.x);

    [unroll(8)]
    for (uint i = 0u; i < g_ghostCount; ++i)
    {
        const float fi = float(i);
        // Deterministic per-index variation. Not a real hash: it only has to be irregular
        // and stable, and a fixed sequence is art-directable.
        const float h1 = frac(sin(fi * 12.9898 + 4.1414) * 43758.5453);
        const float h2 = frac(sin(fi * 78.2330 + 1.7182) * 43758.5453);
        const float h3 = frac(sin(fi * 39.3460 + 2.7182) * 43758.5453);

        // A quarter of the ghosts land on the FAR side of the sun.
        const float dir = (h3 < 0.25) ? -0.55 : 1.0;
        const float t   = (fi + 1.0) * g_ghostDispersal * dir * lerp(0.70, 1.40, h1);

        float2 ghost   = g_sunUV + sunToCenter * (t * 2.0);
        float2 ghostAR = float2((ghost.x - 0.5) * aspect + 0.5, ghost.y);
        ghostAR += ghostPerp * (h3 - 0.5) * 0.035;

        // h2 squared: mostly small ghosts with one or two large faint ones,
        // rather than eight of the same size.
        const float radius = lerp(0.010, 0.090, h2 * h2);
        // Big ghosts are dim, small ones punchy (roughly energy-conserving).
        const float bright = lerp(1.0, 0.25, h2 * h2);

        // Ghosts that wander far past the frame fade out, so the chain never
        // collapses into one hot dot in a corner.
        const float screenFade = 1.0 - smoothstep(0.70, 1.00, distance(ghostAR, ctrAR));
        if (screenFade <= 0.0) continue;

        // Spectral dispersion: integrate the aperture image along a wavelength ramp,
        // displacing radially. A 3-tap RGB offset would show discrete red/blue copies
        // (colour fringing) instead of a prism smear.
        const float spread = g_chromaticOffset * (1.0 + fi * 0.4);
        float3 spectral = 0.0;
        float3 wsum     = 0.0;
        const int kSpectralTaps = 5;
        [unroll]
        for (int s = 0; s < kSpectralTaps; ++s)
        {
            const float st  = (float(s) + 0.5) / float(kSpectralTaps);
            const float3 wt = SpectrumTint(st);
            const float2 off = ghostDir * ((st - 0.5) * spread);
            spectral += wt * ApertureGhost(uvAR, ghostAR + off, radius,
                                           g_apertureBlades, g_apertureRotation);
            wsum     += wt;
        }
        spectral /= max(wsum, 1e-4);

        color += g_sunColor * spectral * bright * screenFade * g_ghostBrightness;
    }

    color *= intensity;

    g_output[id.xy] = float4(color, 1.0);
}
