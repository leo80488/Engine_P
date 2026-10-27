#pragma once

#include <DirectXMath.h>
#include <cstdint>

// LocalFogVolumeComponent - a bounded pocket of participating medium (UE "Local Fog Volume", HDRP
// "Local Volumetric Fog"): mist in a valley, smoke in a room, a glowing cloud around a light.
//
// Bounds come from the entity's GlobalTransform, as for PostProcessVolumeComponent: Box = OBB with
// half-extents = scale, Sphere = ellipsoid with radii = scale (uniform scale -> sphere). Nothing
// spatial is stored here; the Transform gizmo is the editing tool.
//
// The volume ADDS its medium to the global fog (VolumetricFogComponent, or the pass's own settings)
// inside the froxel grid, so it is visible only within the global view distance. The renderer culls
// volumes beyond it on the CPU and keeps the nearest VolumetricFogPass::kMaxLocalFogVolumes.
enum class LocalFogShape : uint8_t
{
    Box,      // OBB, half-extents = scale
    Sphere,   // ellipsoid, radii = scale
};

struct LocalFogVolumeComponent
{
    bool          enabled  = true;
    LocalFogShape shape    = LocalFogShape::Box;

    float density          = 0.1f;   // extinction sigma_t at the core (1/m)
    DirectX::XMFLOAT3 albedo { 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 emissive { 0.0f, 0.0f, 0.0f };
    float emissiveIntensity = 0.0f;

    // Fraction of the half-extent (0..1) over which the density ramps from 0
    // at the boundary to full inside. 0 = hard edge.
    float edgeFade         = 0.25f;
    // Exponential falloff per metre above the volume's BOTTOM face (local -Y),
    // for ground mist that thins upward inside the box. 0 = uniform.
    float heightFalloff    = 0.0f;
};
