// DDGIProbeDebug.ps.hlsl - analytic impostor sphere shading for the probe debug view.
// The VS emits a camera-facing quad per probe; this PS intersects the eye ray with the
// probe sphere. Pixels off the sphere are discarded (round silhouette), the hit normal
// is exact, and SV_Depth is the true sphere depth so scene geometry occludes probes
// (reversed-Z, GREATER_EQUAL test).
// Colour = the probe's stored atlas irradiance along the sphere normal, or one of the
// diagnostic modes in DebugConstants.

#include "DDGICommon.hlsli"
#include "DDGISampling.hlsli"

cbuffer PerViewCB : register(b0, space0)
{
    float4x4 g_ViewProj;             // current, jittered (matches scene depth)
    float4x4 g_PrevViewProj;         // unused here
    float4x4 g_CurViewProjNoJitter;  // unused here
    float4   g_TimeParams;           // unused here
    float4   g_CameraPosWS;          // xyz = camera world position
};

ConstantBuffer<DDGIVolumeGPU>  g_Vol           : register(b1, space0);
Texture2D<float4>              g_Irradiance    : register(t0, space0);
SamplerState                   g_LinearSampler : register(s0, space0);

cbuffer DebugConstants : register(b2, space0)
{
    float g_SphereRadius;
    uint  g_DebugMode;     // 0 = probe irradiance (default)
                           // 1 = grid-coord colour (verify probe placement)
                           // 2 = mean irradiance, +Y/-Y average (verify the atlas has data)
                           // 3 = directional contrast (verify the atlas evolves frame to frame)
    uint  _padR1, _padR2;
};

struct PSIn
{
    float4 pos      : SV_POSITION;
    float3 worldPos : WORLD_POS;
    nointerpolation float3 center   : SPHERE_CENTER;
    nointerpolation uint   probeIdx : PROBE_IDX;
};

struct PSOut
{
    float4 color : SV_TARGET;
    float  depth : SV_Depth;
};

PSOut main(PSIn i)
{
    // ---- Analytic ray-sphere intersection ----
    const float  r   = max(g_SphereRadius, 1e-4);
    const float3 o   = g_CameraPosWS.xyz;
    const float3 dir = normalize(i.worldPos - o);
    const float3 oc  = o - i.center;

    const float b    = dot(oc, dir);
    const float disc = b * b - (dot(oc, oc) - r * r);
    if (disc < 0.0) discard;               // off the silhouette

    float t = -b - sqrt(disc);             // near hit
    if (t < 0.0) t = -b + sqrt(disc);      // camera inside the sphere
    if (t < 0.0) discard;                  // sphere fully behind the camera

    const float3 hit = o + dir * t;
    const float3 n   = normalize(hit - i.center);

    // True sphere depth (reversed-Z clip convention, same jittered viewProj
    // as the scene depth buffer this pass depth-tests against).
    float4 clip = mul(float4(hit, 1.0), g_ViewProj);
    PSOut outp;
    outp.depth = clip.z / clip.w;

    // ---- Diagnostic modes ----
    int3 coord = DDGI_ProbeCoord(i.probeIdx, g_Vol);

    if (g_DebugMode == 1)
    {
        // Grid-coord colour (R=+x, G=+y, B=+z): verifies probe placement without touching
        // atlas data. Half strength so it does not bloom.
        float3 c = float3(
            float(coord.x) / max(float(g_Vol.probeCountsX - 1), 1.0),
            float(coord.y) / max(float(g_Vol.probeCountsY - 1), 1.0),
            float(coord.z) / max(float(g_Vol.probeCountsZ - 1), 1.0));
        outp.color = float4(c * 0.5, 1.0);
        return outp;
    }

    if (g_DebugMode == 2)
    {
        // Mean irradiance: the +Y and -Y taps averaged (an atlas has no ambient coefficient
        // to read, so this approximates it). Zero everywhere -> relight is not writing the
        // atlas.
        float3 up = DDGI_SampleProbeIrradiance(g_Irradiance, g_LinearSampler,
                                               coord, g_Vol, float3(0, 1, 0));
        float3 dn = DDGI_SampleProbeIrradiance(g_Irradiance, g_LinearSampler,
                                               coord, g_Vol, float3(0, -1, 0));
        float3 avg = (up + dn) * 0.5;
        float  m   = length(avg);
        outp.color = float4(avg / (1.0 + m), 1.0);
        return outp;
    }

    if (g_DebugMode == 3)
    {
        // Directional contrast across the probe's sphere: six axis taps, max minus min.
        // All black -> the probe sees the same irradiance in every direction (relight averaged
        // the direction out, or the probe is buried); static under a moving sun -> the atlas
        // is not being rewritten. Six taps also show bimodal lighting (a corridor lit from
        // both ends) that a single dominant direction would report as none.
        float3 lo = 1e30, hi = -1e30;
        const float3 kAxes[6] = { float3( 1, 0, 0), float3(-1, 0, 0),
                                  float3( 0, 1, 0), float3( 0,-1, 0),
                                  float3( 0, 0, 1), float3( 0, 0,-1) };
        [unroll] for (uint a = 0; a < 6; ++a)
        {
            float3 v = DDGI_SampleProbeIrradiance(g_Irradiance, g_LinearSampler,
                                                  coord, g_Vol, kAxes[a]);
            lo = min(lo, v); hi = max(hi, v);
        }
        float3 spread = max(hi - lo, 0.0);
        float  m      = length(spread);
        outp.color = float4(spread / (1.0 + m), 1.0);
        return outp;
    }

    // Default mode: the probe's stored irradiance along the sphere normal - exactly what
    // the atlas holds, with no reconstruction in between.
    float3 irr = DDGI_SampleProbeIrradiance(g_Irradiance, g_LinearSampler,
                                            coord, g_Vol, n);

    // Tone-map to roughly LDR: stored irradiance is HDR (easily 5-10 under sky + sun) and
    // raw values bloom. Reinhard x/(1+x) keeps relative colour; the +0.02 floor keeps
    // new (all-zero) probes visible as faint dark spheres.
    irr += 0.02;
    outp.color = float4(irr / (1.0 + irr), 1.0);
    return outp;
}
