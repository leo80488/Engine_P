#ifndef LIGHT_EVAL_HLSLI
#define LIGHT_EVAL_HLSLI

// light_eval.hlsli - the single implementation of punctual-light falloff.
//
// No consumer re-implements light falloff: hand-written copies drift, and the same
// lamp then looks different in the main view, a mirror, GI and fog. Surfaces
// (Lighting.ps, forward, water, DDGI, RT reflections, PT, RTDI/RTGI) use the
// Surface* forms: quadratic window x inverse square.
//
// The raster fog tier has its own entry point, Volumetric*: window only, no inverse
// square. A froxel sample can land on a light's exact position, where 1/d^2 is
// singular and gives a temporal flicker spike no denoiser hides. The ray-traced froxels
// average the inverse square over the froxel instead (Froxel*, below), and the path
// tracers' media vertices take the Surface* forms: wherever rays light the fog, its
// falloff is physical. Do not move the raster tier off Volumetric*: the fog of every
// raster scene was tuned against it.
//
// The cone term is identical everywhere, so a spot has the same silhouette in the
// main view, a reflection, GI and fog. The two fog shaders (FroxelLightInject,
// VolumetricRaymarch) take the cone from here but keep their own quartic window
// (1 - (d/r)^4)^2 for distance; moving them onto VolumetricDistanceAttenuation's
// quadratic one would darken every fog spot mid-range.
//
// The maths is the GPULight form (`radius`, `color`, `intensity`, `spotAngle`):
//   window:  (1 - (d/r)^2)^2 - exactly 0 at d == r with zero derivative.
//   cone:    smoothstep(cos(outer), cos(outer*0.75), cd), paying the cos().
//   near:    d^2 clamped to 1e-2.
// Changing any of these changes the image of every consumer at once.
// TODO(light_parameters Phase 1): adopt the V2 forms together with the V2 GPULight
// ABI - the quartic window everywhere (fog included), a precomputed cone ramp, and
// sourceRadius in place of the 1e-2 floor.
//
// ==== No resource access in this file ====
// Included by compute shaders (DDGIRayTrace, HybridReflIntersectHW,
// FroxelLightInject, PathTrace, RTDI, RTGI) under several root signatures, so it
// stays pure arithmetic and the per-light shadow sample stays with each caller
// (shadow.hlsli reads the descriptor heap at three sites). A heap read under a
// root signature without CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED compiles, then fails at
// PSO creation with no line number. Every signature running shaders/ sets the flag
// except NRD's and the inline present shader's, which include nothing from here;
// a new signature must set it too (tools/compute_include_probe.cs.hlsl).
//
// cluster_common.hlsli is included here rather than assumed: guarding the GPULight
// overloads on "did the consumer include it?" lets include order silently decide
// whether the overloads exist, and a shader with the wrong order falls back to the
// scalar overloads with no diagnostic.
#include "cluster_common.hlsli"

// ==== Range window ====
// Smooth cutoff that reaches exactly zero at d == range with zero derivative, so a
// light's influence sphere ends without a visible seam and the cluster-culling
// sphere is an exact bound on the lit region. Squaring the window gives C1
// continuity at the boundary; the unsquared form kinks.
// The `saturate` and the `max(range, 1e-4)` are no-ops on every reachable input
// (callers early-out on `dist > radius`, and a radius-0 light lights nothing), but
// they make the function total: a caller that forgets the early-out gets 0, not a
// NaN that spreads over the whole cluster.
float LightRangeWindow(float distanceToLight, float range)
{
    float x = saturate(distanceToLight / max(range, 1e-4));
    float w = saturate(1.0 - x * x);
    return w * w;
}

// ==== Surface distance attenuation ====
// Physical inverse square, with the denominator floored so a surface (or a ray hit)
// on the light's exact position cannot produce INF/NaN.
// The 1e-2 floor is not a fudge for "too bright": fix over-bright lamps with the
// project photometric scale or the authored intensity. Do not change this function
// for one consumer: that re-forks the inconsistency this header removes.
float SurfaceDistanceAttenuation(float distanceToLight, float range)
{
    return LightRangeWindow(distanceToLight, range)
         / max(distanceToLight * distanceToLight, 0.01);
}

// ==== Volumetric distance attenuation ====
// Window only, no inverse square: the renderer's policy for froxel/raymarch fog and
// the only sanctioned difference between the surface and volumetric light models
// (see the file header).
float VolumetricDistanceAttenuation(float distanceToLight, float range)
{
    return LightRangeWindow(distanceToLight, range);
}

// ==== Froxel distance attenuation ====
// The ray-traced froxel lighting (FroxelLightInjectRT) keeps the surfaces' physical form and
// averages the inverse square over the froxel instead of dropping it: 1 / (d^2 + r^2), r the
// froxel's radius (or the light's sphere when larger), is finite and smooth where a light sits
// inside the froxel, which is the flicker the Volumetric* form avoids by window alone.
float FroxelDistanceAttenuation(float distanceToLight, float range, float froxelRadius)
{
    return LightRangeWindow(distanceToLight, range)
         / max(distanceToLight * distanceToLight + froxelRadius * froxelRadius, 1e-4);
}

// ==== Angular (cone) attenuation ====
//   surfaceToLight  normalised vector from the shaded point toward the light.
//   lightDir        direction the light travels (the cone axis, entity +Z).
//   spotHalfAngle   authored outer half-angle, radians. <= 0 -> not a spot.
//
// The inner cone is a fixed 0.75 x outer. The `spotHalfAngle <= 0` early-out is
// part of the contract, not an optimisation: a spot with angle 0 gives
// cos(0) == cos(0), and smoothstep(1, 1, x) is ill-defined.
float AngularAttenuation(float3 surfaceToLight, float3 lightDir, float spotHalfAngle)
{
    if (spotHalfAngle <= 0.0) return 1.0;
    float cd       = dot(-surfaceToLight, normalize(lightDir));
    float cosOuter = cos(spotHalfAngle);
    float cosInner = cos(spotHalfAngle * 0.75);
    return smoothstep(cosOuter, cosInner, cd);
}

// ==== GPULight overloads ====
// `light.type`: 0 = directional (never in the local-light buffer), 1 = point,
// 2 = spot. Only type 2 has a cone, so the overload passes half-angle 0 otherwise:
// a point light must return exactly 1 in every direction.

float AngularAttenuation(float3 surfaceToLight, GPULight light)
{
    return AngularAttenuation(surfaceToLight, light.direction,
                              (light.type == 2) ? light.spotAngle : 0.0);
}

float SurfaceDistanceAttenuation(float distanceToLight, GPULight light)
{
    return SurfaceDistanceAttenuation(distanceToLight, light.radius);
}

float VolumetricDistanceAttenuation(float distanceToLight, GPULight light)
{
    return VolumetricDistanceAttenuation(distanceToLight, light.radius);
}

// Full surface attenuation for one local light: distance x cone.
//   toLight  normalised surface -> light vector
//   dist     the matching unnormalised length
// Multiply the result by the light's radiance (colour x intensity) and by its
// shadow visibility - neither belongs here. Visibility must stay a separate
// multiplicative factor; see VIS-LINEAR in Lighting.ps.hlsl.
float EvalLocalLightAttenuation(GPULight light, float3 toLight, float dist)
{
    return SurfaceDistanceAttenuation(dist, light.radius)
         * AngularAttenuation(toLight, light);
}

// Volumetric counterpart - same cone, window-only distance.
float EvalVolumetricLightAttenuation(GPULight light, float3 toLight, float dist)
{
    return VolumetricDistanceAttenuation(dist, light.radius)
         * AngularAttenuation(toLight, light);
}

#endif // LIGHT_EVAL_HLSLI
