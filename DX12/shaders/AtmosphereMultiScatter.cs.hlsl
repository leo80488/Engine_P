// AtmosphereMultiScatter.cs.hlsl - Hillaire 2020 iterative multi-scatter LUT.
// Output: 32x32 R16G16B16A16_FLOAT, written once after the TransmittanceLUT.
//
// Encodes the multi-scattering response Psi: for an isotropic white light source
// spread uniformly over the sphere, how much light all-orders scattering returns
// after the first bounce. The SkyView pass multiplies m.scattering by it to add
// the multi-scattered part of the sky colour.
//
// Per texel one 1x1x64 threadgroup marches 64 stratified directions (an 8x8 grid,
// 32 steps each) into LDS; thread 0 applies the closed form Psi = L2 / (1 - Fms)
// and writes the texel.

#include "AtmosphereCommon.hlsli"

cbuffer MultiScatterCB : register(b0, space2)
{
    uint lutWidth;
    uint lutHeight;
    uint _pad0;
    uint _pad1;
};

Texture2D<float4>   TransmittanceLUT : register(t0, space2);
RWTexture2D<float4> DstLUT           : register(u0, space2);
SamplerState        LinClamp         : register(s0, space2);

// The paper's parallel-per-pixel layout: 8x8 = 64 directions per threadgroup.
#define MS_SQRT_SAMPLES 8
#define MS_SAMPLE_COUNT (MS_SQRT_SAMPLES * MS_SQRT_SAMPLES)
#define MS_MARCH_STEPS  32

groupshared float3 gs_Lum[MS_SAMPLE_COUNT];
groupshared float3 gs_Fms[MS_SAMPLE_COUNT];

float3 SampleTrans(float h, float mu)
{
    float2 uv; ParamsToLutUv(h, mu, uv);
    return TransmittanceLUT.SampleLevel(LinClamp, uv, 0).rgb;
}

// Stratified uniform sphere sample from (i, j) in [0, N)^2 on a jittered grid.
float3 SphereSampleStratified(uint ix, uint iy)
{
    // Hillaire: (i + 0.5) / N -> a fraction in (0, 1).
    float u = (float(ix) + 0.5) / float(MS_SQRT_SAMPLES);
    float v = (float(iy) + 0.5) / float(MS_SQRT_SAMPLES);

    float phi      = 2.0 * PI * u;
    float cosTheta = 1.0 - 2.0 * v;                      // maps v in (0,1) -> cos(theta) in (-1,1)
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    return float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
}

[numthreads(1, 1, MS_SAMPLE_COUNT)]
void main(uint3 GID : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
    // One 1x1x64 threadgroup per LUT texel. GID is the pixel coord (uniform across
    // the group, so the compiler accepts the barrier below); GTid.z is the sample
    // index 0..63.
    uint pixelX  = GID.x;
    uint pixelY  = GID.y;
    uint sampIdx = GTid.z;

    // Bounds check: all threads in a group share pixelX/pixelY, so the branch is
    // uniform (no varying flow control at the barrier below).
    bool inBounds = (pixelX < lutWidth) && (pixelY < lutHeight);

    float3 L_sample = 0;
    float3 F_sample = 0;

    if (inBounds)
    {
        // ---- Pixel parameters (shared across all 64 threads) ----
        float2 uv = (float2(pixelX, pixelY) + 0.5) / float2(lutWidth, lutHeight);

        float h, mu;
        LutUvToParams(uv, h, mu);

        float  r      = kGroundR + h;
        float3 origin = float3(0.0, r, 0.0);
        float3 sunDir = float3(sqrt(saturate(1.0 - mu * mu)), mu, 0.0);

        // ---- This thread's spherical direction ----
        uint ix = sampIdx % MS_SQRT_SAMPLES;
        uint iy = sampIdx / MS_SQRT_SAMPLES;
        float3 rayDir = SphereSampleStratified(ix, iy);

        // Intersect the atmosphere top / the ground.
        float tAtmo   = RaySphere(origin, rayDir, kAtmoR);
        float tGround = RaySphere(origin, rayDir, kGroundR);
        float tMax    = (tGround > 0.0 && tGround < tAtmo) ? tGround : tAtmo;

        if (tMax > 0.0)
        {
            float dt = tMax / float(MS_MARCH_STEPS);
            float3 T = float3(1, 1, 1);

            [loop] for (uint step = 0; step < MS_MARCH_STEPS; ++step)
            {
                float3 p   = origin + rayDir * (dt * (float(step) + 0.5));
                float  alt = length(p) - kGroundR;
                AtmoMedium m = SampleAtmosphere(alt);

                float3 sampT = exp(-m.extinction * dt);
                float  pMu   = dot(normalize(p), sunDir);

                // Transmittance to the sun (0 when the ground blocks it).
                float3 Tsun = SampleTrans(alt, pMu);
                float  tBlock = RaySphere(p, sunDir, kGroundR);
                if (tBlock > 0.0) Tsun = 0;

                // Energy-conserving scattering integral (Hillaire 2020 eq. 6), with the uniform
                // isotropic phase 1/(4 pi) over the source sphere.
                float3 S     = m.scattering * (1.0 / (4.0 * PI));
                float3 Sint  = (S - S * sampT) / max(m.extinction, float3(1e-6, 1e-6, 1e-6));

                // L (Hillaire eq. 5) keeps the uniform phase on the in-scatter: the radiance
                // into the sample direction is sigma_s * p_u * Tsun, with no 4 pi undo (that
                // would make the MS term ~12.6x too bright). f_ms (eq. 7) integrates
                // sigma_s * T with no phase, so there the 4 pi cancels the p_u baked into S.
                L_sample += T * Tsun * Sint;
                F_sample += T * Sint * (4.0 * PI);
                T        *= sampT;
            }

            // Ground bounce: if the ray hit the ground, add one Lambertian bounce of the
            // sun, attenuated by the transmittance.
            if (tGround > 0.0 && tGround < tAtmo)
            {
                float3 pGround = origin + rayDir * tGround;
                float3 nGround = normalize(pGround);
                float  NdotL   = saturate(dot(nGround, sunDir));
                float3 Tgr     = SampleTrans(0.0, dot(nGround, sunDir));
                float3 Tview   = T; // transmittance to that ground point
                // Lambertian bounce = albedo / pi * NdotL; no trailing * pi, which would cancel
                // the BRDF's 1/pi.
                L_sample += Tview * kGroundAlbedo * (1.0 / PI) * NdotL * Tgr;
            }
        }
    }

    // ---- LDS reduction (all 64 threads must reach the barrier) ----
    gs_Lum[sampIdx] = L_sample;
    gs_Fms[sampIdx] = F_sample;
    GroupMemoryBarrierWithGroupSync();

    if (inBounds && sampIdx == 0)
    {
        float3 L = 0;
        float3 F = 0;
        [unroll] for (uint k = 0; k < MS_SAMPLE_COUNT; ++k)
        {
            L += gs_Lum[k];
            F += gs_Fms[k];
        }
        float invN = 1.0 / float(MS_SAMPLE_COUNT);
        L *= invN;
        F *= invN;

        // Hillaire closed form: Psi = L / (1 - Fms).
        float3 Psi = L / max(float3(1, 1, 1) - F, float3(1e-4, 1e-4, 1e-4));
        DstLUT[uint2(pixelX, pixelY)] = float4(Psi, 1.0);
    }
}
