#ifndef ATMOSPHERE_COMMON_HLSLI
#define ATMOSPHERE_COMMON_HLSLI

// AtmosphereCommon.hlsli - shared constants + helpers for the Hillaire 2020 LUT
// pipeline:
//   - TransmittanceLUT (256x64, once)       : transmittance from (altitude, mu) to TOA
//   - MultiScatterLUT  (32x32, once)        : 2nd+-order scatter factor
//   - SkyViewLUT       (192x108, per frame) : sky radiance by (azimuth, elevation)
// All distances in km.

#include "core/math.hlsli"   // PI

// ==== Earth-scale constants (Hillaire 2020 defaults) ====
static const float  kGroundR      = 6360.0;  // planet radius (km)
static const float  kAtmoR        = 6460.0;  // atmosphere outer radius (km)

static const float3 kRayleighScat = float3(5.802, 13.558, 33.1) * 1e-3;
static const float  kRayleighH    = 8.0;

static const float  kMieScat      = 3.996e-3;
// Bruneton/Hillaire reference: Mie extinction = 4.440e-3 /km, so absorption =
// extinction - scattering = 0.444e-3 /km. Do not use the extinction value as the
// absorption: that is 10x too absorptive across the whole LUT pipeline.
static const float  kMieExt       = 4.440e-3;
static const float  kMieAbsorb    = kMieExt - kMieScat;
static const float  kMieH         = 1.2;
// Cornette-Shanks asymmetry. 0.80 is the Bruneton/Hillaire reference and UE's
// SkyAtmosphere default (MieAnisotropy). A higher g (e.g. 0.92) packs the aerosol
// lobe so tightly around the sun that the haze / glow ring never reaches the rest
// of the dome; the sun's surrounding whitening lives entirely in that lobe.
static const float  kMiePhaseG    = 0.80;

static const float3 kOzoneAbsorb  = float3(0.650, 1.881, 0.085) * 1e-3;
static const float  kOzoneCenter  = 25.0;
static const float  kOzoneHalfWid = 15.0;

static const float3 kGroundAlbedo = float3(0.3, 0.3, 0.3);

// ==== Density / coefficient sampling ====
struct AtmoMedium
{
    float3 scattering;  // Rayleigh + Mie (per-channel)
    float3 extinction;  // scattering + absorption (+ Ozone absorption)
    float3 sctRayleigh; // Rayleigh only (for phase weighting)
    float  sctMie;      // Mie only
};

AtmoMedium SampleAtmosphere(float altitude)
{
    // altitude is km above ground; clamped, since it can be negative underground.
    float h   = max(altitude, 0.0);
    float rDens = exp(-h / kRayleighH);
    float mDens = exp(-h / kMieH);
    float oDens = saturate(1.0 - abs(h - kOzoneCenter) / kOzoneHalfWid);

    AtmoMedium a;
    a.sctRayleigh = kRayleighScat * rDens;
    a.sctMie      = kMieScat * mDens;
    a.scattering  = a.sctRayleigh + float3(a.sctMie, a.sctMie, a.sctMie);
    a.extinction  = a.scattering
                  + float3(kMieAbsorb, kMieAbsorb, kMieAbsorb) * mDens
                  + kOzoneAbsorb * oDens;
    return a;
}

// ==== Phase functions ====
float RayleighPhase(float cosT)
{
    return 3.0 / (16.0 * PI) * (1.0 + cosT * cosT);
}
// Cornette-Shanks phase (despite the HG name), normalised by 3/(8 pi); a 1/(4 pi)
// normalisation integrates to 2/3 over the sphere and dims Mie single-scatter by
// a third.
float MiePhaseHG(float cosT, float g)
{
    float g2   = g * g;
    float num  = (1.0 - g2) * (1.0 + cosT * cosT);
    float den  = (2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-3), 1.5);
    return 3.0 * num / (8.0 * PI * den);
}

// ==== Sphere intersection ====
// Ray origin `ro` inside the sphere (outside also works). Returns the nearest
// positive intersection distance, or -1 if nothing is hit in front.
float RaySphere(float3 ro, float3 rd, float radius)
{
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float d = sqrt(disc);
    float t0 = -b - d;
    float t1 = -b + d;
    if (t1 < 0.0) return -1.0;
    return (t0 < 0.0) ? t1 : t0;
}

// Returns distance to atmosphere top along `rd` from point `ro` that is
// inside the atmosphere. Returns -1 if the ray points down through the ground
// before reaching the atmosphere boundary.
float DistanceToAtmosphereTop(float3 ro, float3 rd)
{
    float tTop   = RaySphere(ro, rd, kAtmoR);
    float tGround = RaySphere(ro, rd, kGroundR);
    if (tGround > 0.0 && tGround < tTop) return -1.0; // hits ground first
    return tTop;
}

// ==== LUT parameterisations (Hillaire 2020) ====
// The transmittance LUT maps (height h in [0, atmosphere thickness], cos(sun
// zenith) mu in [-1, 1]) -> uv in [0, 1]^2.

void ParamsToLutUv(float h, float mu, out float2 uv)
{
    // Height encoded as sqrt for density sensitivity near ground.
    float H = sqrt(kAtmoR * kAtmoR - kGroundR * kGroundR);
    float rho = sqrt(max(0.0, h * (2.0 * kGroundR + h)));
    float u   = rho / H;

    // Parameterise mu by horizon angle for non-linear precision near the horizon.
    float r = kGroundR + h;
    float dMin = kAtmoR - r;
    float dMax = rho + H;
    float d     = max(0.0, -r * mu + sqrt(max(0.0, r * r * (mu * mu - 1.0) + kAtmoR * kAtmoR)));
    float v     = (d - dMin) / max(dMax - dMin, 1e-6);
    uv = float2(u, v);
}

void LutUvToParams(float2 uv, out float h, out float mu)
{
    float H = sqrt(kAtmoR * kAtmoR - kGroundR * kGroundR);
    float rho = H * uv.x;
    h = sqrt(rho * rho + kGroundR * kGroundR) - kGroundR;

    float r = kGroundR + h;
    float dMin = kAtmoR - r;
    float dMax = rho + H;
    float d = dMin + uv.y * (dMax - dMin);
    mu = (d == 0.0) ? 1.0
                    : (H * H - rho * rho - d * d) / max(2.0 * r * d, 1e-6);
    mu = clamp(mu, -1.0, 1.0);
}

// SkyView LUT non-linear elevation parameterisation (more precision near the
// horizon): elevation in [-pi/2, pi/2] -> uv.y in [0, 1].
float SkyViewElevationToV(float elevation)
{
    // Hillaire's sign * sqrt(|x|) pulls the horizon to the middle.
    float sgn = sign(elevation);
    float a   = sqrt(abs(elevation) / (PI * 0.5));
    return 0.5 + 0.5 * sgn * a;
}
float SkyViewVToElevation(float v)
{
    float c   = 2.0 * v - 1.0;
    float sgn = sign(c);
    return sgn * c * c * (PI * 0.5);
}

#endif // ATMOSPHERE_COMMON_HLSLI
