// SSRTemporal.cs.hlsl - SSR history reprojection + neighbourhood clipping (pass 4 of the Hi-Z SSR
// pipeline, after Wicked's ssr_temporalCS). Two reprojection candidates compete:
//   (a) velocity prev UV        - right for surface motion
//   (b) reflection-hit prev UV  - the hit point through prevViewProj, right for moving reflected content
// The one whose history luminance is closer to the current pixel wins; history is then clipped to a
// YCoCg AABB from the 3x3 mean/stddev to suppress ghosting.
// Outputs: temporal colour, (variance, history length) and the current depth, each ping-ponged with
// the previous frame.

cbuffer SSRTemporalCB : register(b0, space2)
{
    // BOTH matrices are jittered. invViewProj is the inverse of the current jittered VP, so unprojecting
    // (uv, depth) recovers the exact rasterized surface position; prevViewProj is the previous jittered
    // VP, so reprojected UVs land on the jittered grid the history texture lives on. Do not pair a
    // jittered invVP with a non-jittered prevVP: the history sample sits ~1 px off every frame and the
    // 0.95 EMA drags it into a ghost trail even on static frames.
    float4x4 invViewProj;           // current inverse (jittered)
    float4x4 prevViewProj;          // previous-frame viewProj (jittered)
    // Dispatch and ping-pong size; equals renderW/H (SSR runs at full res).
    uint     traceW;     uint   traceH;
    float    invTraceW;  float  invTraceH;
    uint     resetHistory;          // 1 = skip history read (frame 0 / resize)
    float    nearZ;      float  farZ;
    uint     renderW;    uint   renderH;
    uint     frameIndex; uint   _pad0;
};

Texture2D<float4>  gColorCurrent    : register(t0, space2);  // resolve color
Texture2D<float4>  gColorHistory    : register(t1, space2);  // prev-frame temporal
Texture2D<float>   gVarianceCurrent : register(t2, space2);  // resolve variance
// Previous-frame variance ping-pong, R16G16_FLOAT:
//   .x = accumulated luminance variance
//   .y = accumulation count N (frames of accepted history, 0 on disocclusion)
Texture2D<float2>  gVarianceHistory : register(t3, space2);
Texture2D<float>   gReprojDepth     : register(t4, space2);  // from resolve
Texture2D<float2>  gVelocity        : register(t5, space2);  // NDC delta
Texture2D<float>   gDepth           : register(t6, space2);  // current depth
Texture2D<float>   gDepthHistory    : register(t7, space2);  // prev-frame depth

RWTexture2D<float4> OutColor        : register(u0, space2);
RWTexture2D<float2> OutVariance     : register(u1, space2);  // (variance, historyLen)
// Written every frame with the current pixel's reverse-Z NDC depth: its ping-pong partner is next
// frame's gDepthHistory. Unwritten, the disocclusion test fails everywhere, history never
// accumulates and the upsample blurs everything as freshly disoccluded.
RWTexture2D<float>  OutDepthHistory : register(u2, space2);

SamplerState gLinClamp : register(s0, space2);

// TAA-independent accumulation: this pass is the only temporal filter SSR can rely on (TAA may be
// off). A fixed EMA with a 1-sigma clip never converges on its own: the box comes from the current
// frame's noisy 3x3 stats, so converged history gets clipped back into the noise every frame. Hence
// a per-pixel accumulation count N (the variance ping-pong's .y):
//   * the blend weight ramps 1/2, 2/3, 3/4 ... -> kMaxHistoryWeight: young pixels converge fast,
//     old ones hold a strong EMA;
//   * the clip box widens with N (kClipGamma*), so trusted history may sit further from the noisy
//     mean. Ghosting stays bounded: disocclusion (depth test) resets N to 0 and gamma to 1 sigma.
static const float kMaxHistoryWeight  = 0.95;   // steady-state history weight
static const float kMaxHistoryFrames  = 32.0;   // N saturation
static const float kClipGammaMin      = 1.0;    // sigma multiplier at N = 0
static const float kClipGammaMax      = 2.75;   // sigma multiplier once trusted
static const float kClipGammaFrames   = 16.0;   // N at which gamma saturates
static const float kDisoccWeight      = 2.0;    // depth rejection sharpness
static const float kDisoccThreshold   = 0.3;

// ==== Helpers ====
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float LinearizeReverseZ(float zNdc)
{
    return LinearizeReverseZ(zNdc, nearZ, farZ);
}

float3 RGBToYCoCg(float3 c)
{
    return float3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                   0.5  * c.r              - 0.5  * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 YCoCgToRGB(float3 c)
{
    float y = c.x, co = c.y, cg = c.z;
    return float3(y + co - cg, y + cg, y - co - cg);
}

// devRatio returns how far outside the box the colour sat (1 = on the surface, > 1 = clipped), so
// the caller can punish hard-clipped history.
float3 ClipAABB(float3 center, float3 color, float3 minC, float3 maxC,
                out float devRatio)
{
    float3 e = 0.5 * (maxC - minC) + 1e-5;
    float3 v = color - center;
    float3 a = abs(v / e);
    float  m = max(a.x, max(a.y, a.z));
    devRatio = m;
    return (m > 1.0) ? (center + v / m) : color;
}

#include "core/color.hlsli"   // Luminance

bool InRange01(float2 uv) { return all(uv > 0.0) && all(uv < 1.0); }

// Reflection-hit reprojection: unproject (uv, reprojDepth) with invViewProj and project through
// prevViewProj (both jittered, see the CB).
float2 ReflectionReproject(float2 uv, float reprojDepth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 clip = float4(ndc, reprojDepth, 1.0);
    float4 wp   = mul(clip, invViewProj);
    wp /= wp.w;

    float4 prevClip = mul(float4(wp.xyz, 1.0), prevViewProj);
    if (prevClip.w <= 1e-3) return uv;
    float2 prevNdc = prevClip.xy / prevClip.w;
    return float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

float GetDisocclusion(float curLinZ, float prevLinZ)
{
    float rel = abs(curLinZ - prevLinZ) / max(curLinZ, 0.01);
    return exp(-rel * kDisoccWeight);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    // Full-res dispatch: one thread per G-buffer pixel.
    if (DTid.x >= traceW || DTid.y >= traceH) return;

    const int2   pixel = int2(DTid.xy);
    const float2 uv    = (float2(pixel) + 0.5) * float2(invTraceW, invTraceH);

    float4 current = gColorCurrent.Load(int3(pixel, 0));
    float  curVar  = gVarianceCurrent.Load(int3(pixel, 0));
    float  depth   = gDepth.Load(int3(pixel, 0));

    // ALWAYS write depth history before any early-out: sky and reset pixels still have valid G-buffer
    // depth, and next frame's disocclusion test needs every texel.
    OutDepthHistory[pixel] = depth;

    // History reset: first frame or after a resize.
    if (resetHistory != 0 || depth <= 0.0)
    {
        OutColor[pixel]    = current;
        OutVariance[pixel] = float2(curVar, 0.0);
        return;
    }

    float curLinZ = LinearizeReverseZ(depth);

    // 3x3 neighbourhood moments for the YCoCg clip.
    float3 m1 = 0, m2 = 0;
    int    n  = 0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        int2 np = clamp(pixel + int2(dx, dy),
                        int2(0, 0),
                        int2(int(traceW) - 1, int(traceH) - 1));
        float4 s = gColorCurrent.Load(int3(np, 0));
        float3 c = RGBToYCoCg(s.rgb);
        m1 += c; m2 += c * c; n++;
    }
    float invN = 1.0 / float(n);
    float3 mean3  = m1 * invN;
    float3 stdev3 = sqrt(abs(m2 * invN - mean3 * mean3));
    // The clip box is built in the accepted-history branch: its width depends on the accumulation
    // count read from the history buffer.

    // Two reprojection candidates.
    float2 motionNDC = gVelocity.Load(int3(pixel, 0));
    float2 motionUV  = float2(0.5, -0.5) * motionNDC;
    float2 prevUV_v  = uv - motionUV;

    float reprojDepth = gReprojDepth.Load(int3(pixel, 0));
    float2 prevUV_r  = ReflectionReproject(uv, reprojDepth);

    float4 histV = gColorHistory.SampleLevel(gLinClamp, prevUV_v, 0);
    float4 histR = gColorHistory.SampleLevel(gLinClamp, prevUV_r, 0);

    float curLum = Luminance(current.rgb);
    float dV = abs(Luminance(histV.rgb) - curLum);
    float dR = abs(Luminance(histR.rgb) - curLum);
    float2 prevUV = (dV < dR && InRange01(prevUV_v)) ? prevUV_v : prevUV_r;
    float4 history = (dV < dR && InRange01(prevUV_v)) ? histV    : histR;

    // Disocclusion via depth history; search the neighbourhood when confidence is too low.
    float prevLinZ = LinearizeReverseZ(
        gDepthHistory.SampleLevel(gLinClamp, prevUV, 0));
    float disocc = GetDisocclusion(curLinZ, prevLinZ);

    if (disocc < kDisoccThreshold && InRange01(prevUV))
    {
        // Search the 3x3 history taps (one trace texel apart) for the best depth match.
        float2 dudv = float2(invTraceW, invTraceH);
        float bestDisocc = disocc;
        float2 bestUV    = prevUV;
        [unroll] for (int sy = -1; sy <= 1; ++sy)
        [unroll] for (int sx = -1; sx <= 1; ++sx)
        {
            float2 testUV = prevUV + float2(sx, sy) * dudv;
            float  tLinZ  = LinearizeReverseZ(
                gDepthHistory.SampleLevel(gLinClamp, testUV, 0));
            float  tDisocc = GetDisocclusion(curLinZ, tLinZ);
            if (tDisocc > bestDisocc) { bestDisocc = tDisocc; bestUV = testUV; }
        }
        prevUV  = bestUV;
        disocc  = bestDisocc;
        history = gColorHistory.SampleLevel(gLinClamp, prevUV, 0);
    }

    float4 result;
    float  resultVar;
    float  histLenOut;

    if (disocc >= kDisoccThreshold && InRange01(prevUV))
    {
        // The accumulation count travels in the variance history's .y. Bilinear sampling across a
        // disocclusion edge gives a fractional N; fine, it only feeds the smooth weights below.
        float2 varHist = gVarianceHistory.SampleLevel(gLinClamp, prevUV, 0);
        float  histLen = clamp(varHist.y, 0.0, kMaxHistoryFrames);
        float  N       = histLen + 1.0;

        // YCoCg AABB clip on history; the box widens as the history earns trust.
        float  gamma  = lerp(kClipGammaMin, kClipGammaMax,
                             saturate(histLen / kClipGammaFrames));
        float3 minYC  = mean3 - gamma * stdev3;
        float3 maxYC  = mean3 + gamma * stdev3;
        float  devRatio;
        float3 histYC = RGBToYCoCg(history.rgb);
        histYC = ClipAABB(mean3, histYC, minYC, maxYC, devRatio);
        history.rgb = YCoCgToRGB(histYC);

        // Clip severity: 0 = history inside the box, 1 = it sat 3x or more outside.
        float clipSev = saturate((devRatio - 1.0) * 0.5);

        // 1/2, 2/3, 3/4 ... ramp capped at the steady-state weight. A hard-clipped history means the
        // reflected content really changed: ease the blend toward current AND decay the trust ramp.
        // Otherwise N keeps growing while the box keeps clamping, gamma widens, and the wider box
        // tolerates ever more ghosting (positive feedback).
        float wHist = min(N / (N + 1.0), kMaxHistoryWeight)
                    * lerp(1.0, 0.85, clipSev);
        result     = lerp(current, history, wHist);
        resultVar  = lerp(curVar, varHist.x, wHist);
        histLenOut = lerp(N, histLen * 0.5, clipSev);
    }
    else
    {
        // Disocclusion: no history, accumulation restarts at zero. Keep the real per-frame variance (raw
        // HDR luminance^2, the scale the upsample's luminance gate consumes): a fixed 1.0 makes the gate
        // far too tight on bright disoccluded pixels. The 1.0 floor keeps the blur engaging on dim content.
        result     = current;
        resultVar  = max(curVar, 1.0);
        histLenOut = 0.0;
    }

    OutColor[pixel]    = max(result, 0.0);
    OutVariance[pixel] = float2(max(resultVar, 0.0), histLenOut);
}
