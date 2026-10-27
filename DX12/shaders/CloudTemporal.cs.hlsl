// Temporal reconstruction of the quarter-resolution cloud raymarch.
// The raymarch (1080p -> 480x270, 48-160 samples per ray over 30-500 m features) is far
// below Nyquist in both domains, and clouds write no velocity, so downstream TAA cannot
// average its dither. This accumulator lets CloudRaymarch advance its 8-entry Halton
// sub-texel offset and IGN ray-start offset per frame: real supersampling, not de-banding.
// Reprojection: clouds have no depth, so the history UV comes from the ray's point in the
// middle of the cloud shell. That finite distance stays sub-pixel for a 1.5-4 km deck under
// rotation and translation, where an infinite-distance (pure rotation) reprojection drifts.
// Ghosting: YCoCg neighbourhood clip (as SSRTemporal) plus a hard reset when the reprojected
// UV leaves the frame or the CPU flags a teleport / resize / first frame.
// Runs between DispatchRaymarch and Composite; the composite reads this output, never the raw
// raymarch.

#define CLOUD_CB_REGISTER register(b0, space2)
#include "CloudCommon.hlsli"

Texture2D<float4>   CloudCurrent : register(t0, space2);  // this frame's raymarch
Texture2D<float4>   CloudHistory : register(t1, space2);  // accumulated, previous frame

SamplerState        LinearClamp  : register(s0, space2);

RWTexture2D<float4> CloudOut     : register(u0, space2);  // accumulated, this frame

// rgb = premultiplied in-scatter, a = transmittance. Only rgb goes through YCoCg;
// transmittance is clipped as a scalar against the same neighbourhood.
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

float3 ClipAABB(float3 center, float3 color, float3 minC, float3 maxC)
{
    float3 e = 0.5 * (maxC - minC) + 1e-5;
    float3 v = color - center;
    float  m = max(abs(v.x / e.x), max(abs(v.y / e.y), abs(v.z / e.z)));
    return (m > 1.0) ? (center + v / m) : color;
}

[numthreads(8, 8, 1)]
void main(uint2 dt : SV_DispatchThreadID)
{
    if (dt.x >= (uint)halfResW || dt.y >= (uint)halfResH) return;

    const float2 qDim = float2(halfResW, halfResH);
    const float2 uv   = (float2(dt) + 0.5) / qDim;

    float4 current = CloudCurrent.Load(int3(int2(dt), 0));

    if (resetHistory != 0.0)
    {
        CloudOut[dt] = current;
        return;
    }

    // ---- Reproject through the middle of the cloud shell ----
    float2 ndc = float2(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0));
    float4 nearW = mul(float4(ndc, 1.0, 1.0), invViewProj);   // reversed-Z near
    float4 farW  = mul(float4(ndc, 0.0, 1.0), invViewProj);
    float3 rayP  = nearW.xyz / nearW.w;
    float3 rayD  = normalize(farW.xyz / farW.w - rayP);

    float tEntry, tExit;
    if (!ShellInterval(cameraPos, rayD, tEntry, tExit))
    {
        // The ray misses the shell: nothing to accumulate, and history here would drag cloud onto
        // clear sky.
        CloudOut[dt] = current;
        return;
    }

    // Clamp the reprojection anchor to the traced span: an unclamped mid-point on a near-horizon
    // ray sits tens of km away, where the shell exit is a planet-curvature artefact unrelated to
    // this texel's cloud.
    float tAnchor = tEntry + 0.5 * min(tExit - tEntry, maxTraceDist);
    float3 anchorW = cameraPos + rayD * tAnchor;

    float4 prevClip = mul(float4(anchorW, 1.0), prevViewProj);
    if (prevClip.w <= 1e-4)
    {
        CloudOut[dt] = current;
        return;
    }
    float2 prevNdc = prevClip.xy / prevClip.w;
    float2 prevUV  = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);

    if (any(prevUV <= 0.0) || any(prevUV >= 1.0))
    {
        CloudOut[dt] = current;      // off-screen last frame -> no history
        return;
    }

    // ---- Neighbourhood statistics for the clip box ----
    float3 m1 = 0.0, m2 = 0.0;
    float  aMin = 1e30, aMax = -1e30;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        int2 np = clamp(int2(dt) + int2(dx, dy),
                        int2(0, 0), int2(int(halfResW) - 1, int(halfResH) - 1));
        float4 s = CloudCurrent.Load(int3(np, 0));
        float3 c = RGBToYCoCg(s.rgb);
        m1 += c; m2 += c * c;
        aMin = min(aMin, s.a); aMax = max(aMax, s.a);
    }
    const float invN = 1.0 / 9.0;
    float3 mean3  = m1 * invN;
    float3 stdev3 = sqrt(abs(m2 * invN - mean3 * mean3));

    // Wide box (2 sigma): the input is deliberately stochastic, and a tight box would clip the
    // converged history back into this frame's noise so it never converges.
    const float kGamma = 2.0;
    float3 minYC = mean3 - kGamma * stdev3;
    float3 maxYC = mean3 + kGamma * stdev3;

    float4 history = CloudHistory.SampleLevel(LinearClamp, prevUV, 0);
    history.rgb = YCoCgToRGB(ClipAABB(mean3, RGBToYCoCg(history.rgb),
                                      minYC, maxYC));
    history.a   = clamp(history.a, aMin, aMax);

    float w = saturate(temporalBlend);
    CloudOut[dt] = float4(max(lerp(current.rgb, history.rgb, w), 0.0),
                          saturate(lerp(current.a, history.a, w)));
}
