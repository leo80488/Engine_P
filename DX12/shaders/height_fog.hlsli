#ifndef HEIGHT_FOG_HLSLI
#define HEIGHT_FOG_HLSLI

// height_fog.hlsli - UE-style analytic exponential height fog, one copy of the
// math for the opaque composite (HeightFogApply.ps.hlsl) and for every surface
// drawn after it (fog_apply.hlsli), so the two cannot drift.
// No register bindings: the caller assembles HeightFogParams from its own CB.
//
// Density along the view ray p(t) = O + D*t (world metres, Y-up):
//   sigma(t) = d0 * exp(-k * (O.y + D.y*t - H))
// has the closed-form optical depth over [t0, t1] (len = t1 - t0):
//   tau = d0*exp(-k*(O.y + D.y*t0 - H)) * (1 - exp(-k*D.y*len)) / (k*D.y)
// with the D.y -> 0 limit handled by a Taylor branch. The sky uses the analytic
// t -> inf limit: tau_inf = startDensity / (k*D.y) for climbing rays, divergent
// (-> MaxOpacity) for level/descending rays - no arbitrary "sky distance"
// tunable, and no seam against far geometry.
//
// exp() arguments are clamped to +/-80 everywhere: looking down from far above
// the layer makes -k*relY large, and an unclamped exp() is inf -> NaN after the
// divide.

struct HeightFogParams
{
    float  density;          // d0: extinction at h == height (1/m)
    float  heightFalloff;    // k (1/m)
    float  height;           // H, world metres
    float  startDistance;    // metres of fog-free range in front of the camera
    float3 color;            // ambient inscatter (linear HDR)
    float  maxOpacity;       // UE FogMaxOpacity (0..1)
    float  sunInscatter;     // 0 disables the sun term
    float  anisotropy;       // HG g toward the sun
};

// Henyey-Greenstein - the same function and constants as FroxelCommon.hlsli's
// PhaseHG, kept here so a forward shader does not pull in the froxel
// pipeline's header for one function.
float HeightFog_PhaseHG(float cosTheta, float g)
{
    float g2 = g * g;
    float den = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * 3.14159265358979 * pow(max(den, 1e-3), 1.5));
}

// Optical depth from the camera to a surface `dist` metres along the unit
// `rayDir`. False when the surface is inside the fog-free start range (no fog).
bool HeightFog_TauToSurface(HeightFogParams f, float3 cameraPos, float3 rayDir, float dist,
                            out float tau)
{
    tau = 0.0;
    float t0  = min(f.startDistance, dist);
    float len = dist - t0;
    if (len <= 0.0) return false;

    float relY = cameraPos.y + rayDir.y * t0 - f.height;
    float startDensity = f.density
                       * exp(clamp(-f.heightFalloff * relY, -80.0, 80.0));
    float kdy = f.heightFalloff * rayDir.y;
    float u   = kdy * len;

    tau = (abs(u) > 1e-3)
        ? startDensity * (1.0 - exp(clamp(-u, -80.0, 80.0))) / kdy
        : startDensity * len * (1.0 - 0.5 * u);   // D.y ~ 0 Taylor limit
    return true;
}

// Optical depth to the sky (t -> inf): converges only for rays climbing out of
// the layer; level or descending rays accumulate unbounded fog (-> MaxOpacity).
float HeightFog_TauToSky(HeightFogParams f, float3 cameraPos, float3 rayDir)
{
    float relY = cameraPos.y + rayDir.y * f.startDistance - f.height;
    float startDensity = f.density
                       * exp(clamp(-f.heightFalloff * relY, -80.0, 80.0));
    float kdy = f.heightFalloff * rayDir.y;
    return (kdy > 1e-6) ? startDensity / kdy : 1e9;
}

// (inscatter * opacity, opacity): premultiplied, composited as
// C' = C * (1 - opacity) + rgb - HeightFogApply's ONE / INV_SRC_ALPHA blend.
// dirToSun = -LightCB.lightDir (lightDir points away from the light).
float4 HeightFog_Resolve(HeightFogParams f, float tau, float3 rayDir,
                         float3 dirToSun, float3 sunColor)
{
    tau = max(tau, 0.0);
    float T       = exp(-min(tau, 80.0));
    float opacity = min(1.0 - T, f.maxOpacity);

    float3 inscatter = f.color
                     + sunColor * f.sunInscatter
                       * HeightFog_PhaseHG(dot(rayDir, dirToSun), f.anisotropy);

    return float4(inscatter * opacity, opacity);
}

// The fog in front of a surface at world position `wp` (radial distance from
// the camera), premultiplied as HeightFog_Resolve.
float4 HeightFog_AtSurface(HeightFogParams f, float3 cameraPos, float3 wp,
                           float3 dirToSun, float3 sunColor)
{
    float3 d    = wp - cameraPos;
    float  dist = max(length(d), 1e-4);
    float3 rayDir = d / dist;

    float tau;
    if (!HeightFog_TauToSurface(f, cameraPos, rayDir, dist, tau))
        return float4(0, 0, 0, 0);
    return HeightFog_Resolve(f, tau, rayDir, dirToSun, sunColor);
}

#endif // HEIGHT_FOG_HLSLI
