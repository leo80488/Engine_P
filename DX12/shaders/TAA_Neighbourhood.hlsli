#ifndef TAA_NEIGHBOURHOOD_HLSLI
#define TAA_NEIGHBOURHOOD_HLSLI

#include "TAA_Common.hlsli"

// One 3x3 traversal that produces:
//   (A) Karis luma-weighted AABB statistics (mean + variance) in YCoCg, tonemapped
//       when TAA_USE_TONEMAP_BLEND. Each sample weighs 1/(1+Luminance(s)), so one
//       firefly cannot blow the AABB open and let history fireflies survive the clip.
//   (B) The Blackman-Harris de-jittered current sample in HDR (Karis 2014): a
//       Gaussian exp(-2.29*d^2) around the jitter-corrected texel centre
//       reconstructs the unjittered value, with the same Karis weight against
//       fireflies.
//   (C) HDR means (m1 inverse-tonemapped, and the unweighted 1/9 mean), unweighted
//       Y / luma extremes, the cardinal sum and the raw centre tap.

struct NeighbourhoodStats
{
    float3 m1;                      // YCoCg mean (Karis-weighted; tonemapped if TAA_USE_TONEMAP_BLEND)
    float3 sigma;                   // per-channel stddev (matching m1's space)
    float3 currFilt;                // de-jittered current sample (always HDR linear)
    float3 meanRGB_HDR;             // m1 returned to HDR linear RGB (unread: the firefly clamp uses the unweighted mean)
    float  centerY;                 // YCoCg.x of the centre tap (same space as m1)
    float  yMin;                    // unweighted min of YCoCg.x over 3x3 (same space as m1):
                                    // the symmetric bimodal detector's reference (m1.x is Karis-biased)
    float  yMax;                    // unweighted max of YCoCg.x over 3x3 (same space as m1)
    float  lumaMinHDR;              // unweighted min of HDR-linear luma over 3x3, for the
                                    // tonemap-invariant contrast (max-min)/mid; tonemapped sigma
                                    // compresses HDR-bright bimodal patterns
    float  lumaMaxHDR;              // unweighted max of HDR-linear Luma over 3x3
    float3 meanRGB_HDR_unweighted;  // 1/9 unweighted HDR mean, the firefly clamp's reference
    float3 cardinalSum;             // sum of the 4 cardinal HDR taps, unaveraged, for the
                                    // Karis 5-tap unsharp: sharp = curr*5 - cardinalSum
    float3 centerHDR;               // raw centre tap in HDR linear (high-freq-static path)
};

NeighbourhoodStats ComputeNeighbourhood(Texture2D<float4> currHDR,
                                         int2  pos,
                                         int2  dim,
                                         float jitX,
                                         float jitY)
{
    float3 m1     = 0.0;
    float3 m2     = 0.0;
    float  statsW = 0.0;

    float3 currFilt = 0.0;
    float  currW    = 0.0;

    float3 unweightedSum = 0.0;
    float  centerY       = 0.0;
    float  yMin          =  1e30;
    float  yMax          = -1e30;
    float  lumaMinHDR    =  1e30;
    float  lumaMaxHDR    = -1e30;
    float3 cardinalSum   = 0.0;
    float3 centerHDR     = 0.0;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 sp = clamp(pos + int2(dx, dy), int2(0, 0), dim - 1);
            float3 s = currHDR.Load(int3(sp, 0)).rgb;

            unweightedSum += s;

            // HDR-linear luma min / max (tonemap-invariant), kept in HDR space whatever
            // TAA_USE_TONEMAP_BLEND is; used by the HDR contrast gate.
            {
                float lumaH = Luminance(s);
                lumaMinHDR  = min(lumaMinHDR, lumaH);
                lumaMaxHDR  = max(lumaMaxHDR, lumaH);
            }

            // Cardinals only (4-tap cross): used by Karis 5-tap unsharp downstream.
            if ((dx == 0) ^ (dy == 0))
                cardinalSum += s;
            if (dx == 0 && dy == 0)
                centerHDR = s;

            // (A) Karis luma weight: limits a firefly's influence on the AABB. In tonemap-blend
            // mode the stats live in tonemapped YCoCg and the weight uses the same tonemapped
            // luma, keeping it in [0.5, 1]; an HDR-luma weight falls to ~0.02 on bright pixels,
            // over-deweighting legitimate highlights and shrinking the AABB.
#if TAA_USE_TONEMAP_BLEND
            float3 sT    = ToneMapLuma(s);
            float  lumaW = 1.0 / (1.0 + Luminance(sT));
            float3 ycocg = RGBToYCoCg(sT);
#else
            float  lumaW = 1.0 / (1.0 + Luminance(s));
            float3 ycocg = RGBToYCoCg(s);
#endif

            // The centre tap's Y in m1's space, for the symmetric bimodal detector (centre vs
            // neighbourhood extremes).
            if (dx == 0 && dy == 0)
                centerY = ycocg.x;

            // Unweighted Y min / max: the bimodal detector's reference (the Karis-weighted
            // m1.x is biased dim).
            yMin = min(yMin, ycocg.x);
            yMax = max(yMax, ycocg.x);

            m1     += ycocg * lumaW;
            m2     += ycocg * ycocg * lumaW;
            statsW += lumaW;

            // (B) Karis 2014 Blackman-Harris reconstruction, Gaussian approximation
            //     w = exp(-2.29 * d^2), d = jitter-corrected distance from sample to texel centre.
            // Energy concentrates at the centre tap (no-jitter weights: centre 1.0, cardinal
            // 0.101, corner 0.010), so single-frame HDR sub-pixel peaks stay at their centre
            // tap instead of walking across the 3x3 as the jitter rotates (less specular
            // shimmer). A separable tent, max(0, 1-|dx-jX|) * max(0, 1-|dy-jY|), spreads
            // weight over the whole footprint and, integrated over many frames, visibly
            // low-passes textures.
            float2 dpos = float2(float(dx) - jitX, float(dy) - jitY);
            float  w    = exp(-2.29 * dot(dpos, dpos)) * lumaW;
            currFilt += s * w;
            currW    += w;
        }
    }

    NeighbourhoodStats st;
    st.m1                       = m1 / statsW;
    float3 m2n                  = m2 / statsW;
    st.sigma                    = sqrt(max(0.0, m2n - st.m1 * st.m1));
    st.currFilt                 = currFilt / max(currW, 1e-5);
    st.centerY                  = centerY;
    st.yMin                     = yMin;
    st.yMax                     = yMax;
    st.lumaMinHDR               = lumaMinHDR;
    st.lumaMaxHDR               = lumaMaxHDR;
    st.meanRGB_HDR_unweighted   = unweightedSum * (1.0 / 9.0);
    st.cardinalSum              = cardinalSum;
    st.centerHDR                = centerHDR;
#if TAA_USE_TONEMAP_BLEND
    st.meanRGB_HDR = InvToneMapLuma(YCoCgToRGB(st.m1));
#else
    st.meanRGB_HDR = YCoCgToRGB(st.m1);
#endif
    return st;
}

#endif // TAA_NEIGHBOURHOOD_HLSLI
