#pragma once

// SunTransmittance - CPU mirror of AtmosphereTransmittance.cs.hlsl.
//
// An authored white directional light is the sun's colour above the atmosphere; Rayleigh
// scattering warms what reaches the ground, most at low elevations (UE's SkyAtmosphere applies
// the same term to its atmosphere sun light). TransmittanceLUT on the GPU feeds only the
// sky-view / aerial passes, and a readback would need a GPU->CPU fence, so the same 40-step
// integral runs here for the one sun direction (scalar math, once per frame) and is folded into
// LightCB.lightColor: deferred, DDGI, volumetrics, fog and NPR all inherit the tinted sun.
//
// Contract: the constants and the integration must match shaders/AtmosphereCommon.hlsli and
// shaders/AtmosphereTransmittance.cs.hlsl; retune both together or the sun disagrees with the sky.

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace Atmo
{
    // ==== Earth-scale constants (Hillaire 2020), mirror of AtmosphereCommon.hlsli ====
    inline constexpr float kGroundR = 6360.0f;   // planet radius (km)
    inline constexpr float kAtmoR   = 6460.0f;   // atmosphere outer radius (km)

    inline constexpr float kRayleighScatR = 5.802e-3f;
    inline constexpr float kRayleighScatG = 13.558e-3f;
    inline constexpr float kRayleighScatB = 33.1e-3f;
    inline constexpr float kRayleighH     = 8.0f;

    inline constexpr float kMieScat   = 3.996e-3f;
    inline constexpr float kMieExt    = 4.440e-3f;
    inline constexpr float kMieAbsorb = kMieExt - kMieScat;
    inline constexpr float kMieH      = 1.2f;

    inline constexpr float kOzoneAbsorbR = 0.650e-3f;
    inline constexpr float kOzoneAbsorbG = 1.881e-3f;
    inline constexpr float kOzoneAbsorbB = 0.085e-3f;
    inline constexpr float kOzoneCenter  = 25.0f;
    inline constexpr float kOzoneHalfWid = 15.0f;

    inline constexpr int   kTransmittanceSteps = 40;

    // Nearest positive intersection of ray (ro, rd) with a sphere of `radius`
    // centred at the origin, or -1 when the ray never hits in front.
    inline float RaySphere(const DirectX::XMFLOAT3& ro,
                           const DirectX::XMFLOAT3& rd, float radius)
    {
        const float b = ro.x * rd.x + ro.y * rd.y + ro.z * rd.z;
        const float c = ro.x * ro.x + ro.y * ro.y + ro.z * ro.z - radius * radius;
        const float disc = b * b - c;
        if (disc < 0.0f) return -1.0f;
        const float d  = std::sqrt(disc);
        const float t0 = -b - d;
        const float t1 = -b + d;
        if (t1 < 0.0f) return -1.0f;
        return (t0 < 0.0f) ? t1 : t0;
    }

    // Distance to the top of the atmosphere, or -1 when the ray hits the planet first (the sun is
    // below the horizon from this point).
    inline float DistanceToAtmosphereTop(const DirectX::XMFLOAT3& ro,
                                         const DirectX::XMFLOAT3& rd)
    {
        const float tTop    = RaySphere(ro, rd, kAtmoR);
        const float tGround = RaySphere(ro, rd, kGroundR);
        if (tGround > 0.0f && tGround < tTop) return -1.0f;
        return tTop;
    }

    // Transmittance from a point altitudeKm above ground to the top of the atmosphere, along a
    // direction whose cosine to zenith is muSun. Returns (0,0,0) when the planet blocks the path
    // (a below-horizon sun), so a time-of-day sunset fades out instead of popping.
    inline DirectX::XMFLOAT3 TransmittanceToTop(float muSun, float altitudeKm = 0.0f)
    {
        const float r = kGroundR + std::max(altitudeKm, 0.0f);
        const float mu = std::clamp(muSun, -1.0f, 1.0f);

        // Same 2D setup as the shader: up is +Y, the ray lies in the XY plane.
        const DirectX::XMFLOAT3 p0{ 0.0f, r, 0.0f };
        const DirectX::XMFLOAT3 rd{ std::sqrt(std::max(0.0f, 1.0f - mu * mu)), mu, 0.0f };

        const float tMax = DistanceToAtmosphereTop(p0, rd);
        if (tMax < 0.0f) return { 0.0f, 0.0f, 0.0f };

        const float dt = tMax / static_cast<float>(kTransmittanceSteps);
        float odR = 0.0f, odG = 0.0f, odB = 0.0f;

        for (int i = 0; i < kTransmittanceSteps; ++i)
        {
            const float t = dt * (static_cast<float>(i) + 0.5f);
            const float px = rd.x * t;
            const float py = r + rd.y * t;
            const float alt = std::max(0.0f, std::sqrt(px * px + py * py) - kGroundR);

            const float rDens = std::exp(-alt / kRayleighH);
            const float mDens = std::exp(-alt / kMieH);
            const float oDens = std::clamp(
                1.0f - std::fabs(alt - kOzoneCenter) / kOzoneHalfWid, 0.0f, 1.0f);

            // extinction = Rayleigh scat + Mie scat + Mie absorb + Ozone absorb
            const float mieTerm = (kMieScat + kMieAbsorb) * mDens;
            odR += (kRayleighScatR * rDens + mieTerm + kOzoneAbsorbR * oDens) * dt;
            odG += (kRayleighScatG * rDens + mieTerm + kOzoneAbsorbG * oDens) * dt;
            odB += (kRayleighScatB * rDens + mieTerm + kOzoneAbsorbB * oDens) * dt;
        }

        return { std::exp(-odR), std::exp(-odG), std::exp(-odB) };
    }

    // Transmittance along a world-space direction towards the sun. World up is +Y (as in every
    // atmosphere shader), so only Y matters: transmittance is azimuth-invariant.
    inline DirectX::XMFLOAT3 SunTransmittance(const DirectX::XMFLOAT3& dirToSun,
                                              float altitudeKm = 0.0f)
    {
        const float len = std::sqrt(dirToSun.x * dirToSun.x +
                                    dirToSun.y * dirToSun.y +
                                    dirToSun.z * dirToSun.z);
        const float mu = (len > 1e-6f) ? (dirToSun.y / len) : 1.0f;
        return TransmittanceToTop(mu, altitudeKm);
    }
}
