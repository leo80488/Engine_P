#ifndef DEPTH_COMMON_HLSLI
#define DEPTH_COMMON_HLSLI

// DepthCommon.hlsli - froxel slice mapping and soft-particle fade on top of
// the engine's hardware depth buffer. Used by VolumetricApply, HeightFogApply,
// fog_apply.hlsli (forward surfaces), Tracer and EditorGrid.
//
// All callers use REVERSED-Z: NDC z=1 at the near plane, NDC z=0 at the far
// plane (float precision near 0.0 offsets the 1/z falloff in the distance);
// the renderer's projection builders swap near/far.

// The depth conventions and the view-Z / world-position / view-ray helpers
// (LinearizeReverseZ, WorldPosFromDepth, ViewRayFromUV) live in core/depth.hlsli.
#include "core/depth.hlsli"

// ---- View-Z -> froxel slice (exponential distribution) ----
// The Hillaire / standard volumetric froxel parameterisation:
//   slice 0       -> froxelNear
//   slice depth-1 -> froxelFar
//
// Forward (view-Z -> 0..1 UVW.z):
float ViewZToFroxelW(float viewZ, float froxelNear, float froxelFar)
{
    float t = log(viewZ / froxelNear) / log(froxelFar / froxelNear);
    return saturate(t);
}

// Inverse (slice index 0..depth -> view-Z).
float FroxelSliceToViewZ(float slice, float froxelNear, float froxelFar, uint depth)
{
    float t = slice / float(depth);
    return froxelNear * pow(froxelFar / froxelNear, t);
}

// ---- The scattering volume's W for a surface at planar view-Z `viewZ` ----
// What VolumetricApply.ps.hlsl samples, and what fog_apply.hlsli samples for a
// forward surface: one function, so the two agree. Slice k holds the integral to
// the END of the slice, depth slice k + 1 (FroxelScatter), at its texel's centre:
// a depth's W is half a slice short of it, or trilinear filtering returns the
// integral half a slice past the surface. The slice is offset by +-0.5 slice with
// interleaved-gradient noise (Jorge Jimenez) at the pixel, so trilinear filtering
// averages two adjacent slices in a random pattern and the discrete Z-slice
// boundaries (hard rings on spot cones) read as a gradient.
float FroxelDitherIGN(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float FroxelApplyW(float viewZ, float2 pixel, float froxelNear, float froxelFar, uint froxelDepth)
{
    float w      = ViewZToFroxelW(viewZ, froxelNear, froxelFar) - 0.5 / float(froxelDepth);
    float dither = (FroxelDitherIGN(pixel) - 0.5) / float(froxelDepth);
    return saturate(w + dither);
}

// ---- Soft-particle depth fade ----
// Returns a 0..1 multiplier that fades a transparent surface to 0 as it nears
// the geometry behind it. `pixelNdcZ` is the surface's hardware depth
// (SV_Position.z), `sceneNdcZ` the depth buffer at the same texel, `fadeRange`
// the view-space distance in metres at which fading stops.
//
// Reverse-Z aware: both depths go to view-Z first, so the comparison is in
// metres. A surface coplanar with the scene -> 0 (fully faded), one `fadeRange`
// or more in front of it -> 1; one behind it fails the depth test anyway.
float SoftParticleFade(float pixelNdcZ, float sceneNdcZ,
                       float nearZ, float farZ, float fadeRange)
{
    float pixelView = LinearizeReverseZ(pixelNdcZ, nearZ, farZ);
    float sceneView = LinearizeReverseZ(sceneNdcZ, nearZ, farZ);
    // Positive when the scene is farther away than the surface (normal case).
    float diff = sceneView - pixelView;
    return saturate(diff / max(fadeRange, 1e-4));
}

#endif // DEPTH_COMMON_HLSLI
