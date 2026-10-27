// SSRUpsample.cs.hlsl - variance-driven a-trous bilateral, the final SSR stage (pass 5 of the Hi-Z
// SSR pipeline, after Wicked's ssr_upsampleCS). SSRUpsamplePass dispatches it twice (stepSize 1
// then 2) so SSR has its own spatial denoiser and does not depend on TAA; each iteration reads the
// previous one's output through t0, growing the footprint to a +-9 px cross at constant tap count.
// Weights are edge-stopped by depth-plane distance, normal, roughness and a variance-scaled
// luminance term (SVGF-style): high-variance pixels blend large luminance deltas (noise flattens),
// converged ones blend only near-identical taps (detail survives).
// Output (RGBA16F): the final SSR colour LightingPass reads (one frame late) and SSRComposite adds
// into HDR.

cbuffer SSRUpsampleCB : register(b0, space2)
{
    float4x4 invViewProj;
    uint     screenW;      uint   screenH;
    float    invScreenW;   float  invScreenH;
    float    nearZ;        float  farZ;
    uint     stepSize;     uint   _pad1;    // a-trous hole size (1, 2, 4 ...)
};

Texture2D<float4>  gTemporal   : register(t0, space2);  // iter 0: temporal color; iter 1+: prev iteration
Texture2D<float2>  gVariance   : register(t1, space2);  // temporal variance (.x) + accum count (.y)
Texture2D<float>   gDepth      : register(t2, space2);
Texture2D<float4>  gNormal     : register(t3, space2);
Texture2D<float4>  gSurface    : register(t4, space2);  // roughness in .r

RWTexture2D<float4> OutColor   : register(u0, space2);

SamplerState gLinClamp : register(s0, space2);

static const float kDepthThreshold    = 10000.0;
static const float kNormalThreshold   = 1.0;
// High-variance gate: low enough that the strong blur engages on glossy 1-spp noise (including
// disoccluded small surfaces), not only on extreme outliers.
static const float kVarianceEstimate  = 0.008;
static const float kVarianceExit      = 0.005;   // min variance to blur at all
static const int   kMinRadius         = 0;
// This bilateral is the ONLY denoiser for pixels the temporal pass disoccluded (variance floored at
// 1.0): mostly small reflective surfaces whose reprojected history keeps landing off-surface, so
// accumulation never starts, and a +-2 radius leaves their 1-spp trace noise grainy. The blur stays
// variance-gated (converged surfaces skip it) and depth/normal-weighted (off-surface taps get ~0),
// so the width does not bleed across silhouettes. Two iterations (stepSize 1, 2) at radius 3 give
// +-(3*1 + 3*2) = +-9 px per axis.
static const int   kMaxRadius         = 3;
static const float kBilateralSigma    = 0.9;
// SVGF-style luminance edge-stop scale: sigma_l = kLumSigma * sqrt(variance). Taps whose luminance
// differs from the center by >> sigma_l are rejected: generous at high variance (young /
// disoccluded pixels), tight once converged, so the filter leaves detail alone.
static const float kLumSigma          = 4.0;

#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float LinearizeReverseZ(float zNdc)
{
    return LinearizeReverseZ(zNdc, nearZ, farZ);
}

#include "core/color.hlsli"   // Luminance

float3 ReconstructWorldPos(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, invViewProj);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= screenW || DTid.y >= screenH) return;
    const int2   pixel = int2(DTid.xy);
    const float2 uv    = (float2(pixel) + 0.5) * float2(invScreenW, invScreenH);

    float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0) { OutColor[pixel] = 0; return; }

    float  roughness = max(gSurface.Load(int3(pixel, 0)).r, 0.045);
    float3 N         = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);

    float4 outputColor = gTemporal.SampleLevel(gLinClamp, uv, 0);
    float2 varN        = gVariance.SampleLevel(gLinClamp, uv, 0);
    // Effective variance of the ACCUMULATED signal. .x is the per-frame spatial variance of the
    // stochastic input, which stays high on bright glossy content however converged the history is;
    // dividing by the accumulation count N (.y) gives the variance of the temporal mean. Fresh
    // disocclusions (N = 0) get the full per-frame variance (wide gate, strong blur); converged pixels
    // see the gate shrink, so the a-trous stops eating detail once history has denoised.
    float  variance    = varN.x / (1.0 + varN.y);

    // Variance-driven radius: strong blur on high variance, none on clean pixels.
    bool strongBlur = variance > kVarianceEstimate;
    float radiusF = strongBlur ? float(kMaxRadius) : float(kMinRadius);
    radiusF = lerp(0.0, radiusF, saturate(roughness * 8.0));   // roughness 0.125 = full

    float sigma = radiusF * kBilateralSigma;
    int effRadius = min((int)(sigma * 2.0), (int)radiusF);

    [branch]
    if (variance > kVarianceExit && effRadius > 0)
    {
        float3 P = ReconstructWorldPos(uv, depth);
        float  linDepth = LinearizeReverseZ(depth);
        float  centerLum = Luminance(outputColor.rgb);
        float  sigmaL    = kLumSigma * sqrt(max(variance, 0.0)) + 1e-4;

        float4 result = 0;
        float  wSum = 0;

        // Cross-shaped (H then V) in one dispatch, as Wicked does; the second a-trous iteration at
        // stepSize 2 fills in the diagonals.
        [unroll] for (uint d = 0; d < 2; ++d)
        {
            const int2 dir = d == 0 ? int2(1, 0) : int2(0, 1);
            [loop] for (int r = -effRadius; r <= effRadius; ++r)
            {
                int2 np = pixel + dir * (r * (int)stepSize);
                if (any(np < 0) || np.x >= (int)screenW || np.y >= (int)screenH) continue;

                float sDepth = gDepth.Load(int3(np, 0));
                if (sDepth <= 0.0) continue;

                float2 sUV = (float2(np) + 0.5) * float2(invScreenW, invScreenH);
                float3 sP  = ReconstructWorldPos(sUV, sDepth);
                float3 sN  = normalize(gNormal.Load(int3(np, 0)).rgb * 2.0 - 1.0);

                // Plane-aligned depth error (both surface and sample normals).
                float3 dq = P - sP;
                float  planeErr = max(abs(dot(dq, sN)), abs(dot(dq, N)));
                float  depthRel = planeErr / max(linDepth * farZ, 1e-3);
                float  bDepth   = exp(-depthRel * depthRel * kDepthThreshold);

                float  nErr    = pow(saturate(dot(sN, N)), 4.0);
                float  bNormal = saturate(1.0 - (1.0 - nErr) * kNormalThreshold);

                // Material-similarity term: depth + normal cannot separate mirror water from the flat shoreline
                // terrain touching it (planes coincide at the contact, both normals point up). Terrain above the
                // trace's roughness cutoff is never traced, so its temporal texels are zeros, and a wide blur
                // (variance stays ~1 for dozens of frames after a contact-line disocclusion) drags water confidence
                // down in a band the water shader back-fills with bright sky. Do not blur across roughness
                // discontinuities.
                float  sRough  = max(gSurface.Load(int3(np, 0)).r, 0.045);
                float  bRough  = exp(-abs(sRough - roughness) * 16.0);

                float4 sTap = gTemporal.SampleLevel(gLinClamp, sUV, 0);

                // Variance-scaled luminance edge-stop (SVGF): noise-sized deltas blend, structure-sized deltas
                // survive.
                float  bLum = exp(-abs(Luminance(sTap.rgb) - centerLum) / sigmaL);

                float  g = exp(-(float)(r * r) / max(sigma * sigma, 1e-5));
                float  w = (r == 0) ? 1.0 : g * bDepth * bNormal * bRough * bLum;

                result += sTap * w;
                wSum   += w;
            }
        }

        if (wSum > 1e-5) outputColor = result / wSum;
    }

    OutColor[pixel] = outputColor;
}
