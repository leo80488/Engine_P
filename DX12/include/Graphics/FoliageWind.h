#pragma once

// FoliageWind - CPU mirror of the foliage sway, so ray tracing sees wind-animated
// foliage.
//
// World-Position-Offset is a vertex-shader effect and a BLAS is built from the
// vertex buffer, so ray tracing sees the undeformed mesh: raster shadows sway (WPO
// reaches the shadow depth pass via CUSTOM_VS_SHADOW) while RT shadows, RTAO and
// RTR would keep a rigid silhouette. Baking WPO into a vertex buffer costs one BLAS
// rebuild per instance per frame (thousands for a grass field).
//
// The main bend in asset/Shaders/foliage_wind.hlsl,
//     offset.xz = dirXZ * gust * strength * (worldPos.y - originY),
// is linear in height, i.e. a shear, which a TLAS instance's 3x4 affine transform
// represents exactly; `gust` depends only on time and the instance origin. So the
// main sway goes into the TLAS instance matrix exactly, for the cost of a
// transform write that happens every frame anyway.
//
// The leaf-flutter term is not covered: its phase comes from the mesh-local vertex
// position, so it is per-vertex and not affine. The limit is the ray origin:
// secondary rays (path tracer, ReSTIR GI, RTR) start only 1-2.4 mm off the
// rasterized surface, so a leaf moved off its traced copy along the normal by more
// than that hits the copy and shadows itself. The flutter therefore moves each
// leaf within its own plane (foliage_wind.hlsl), where the unfluttered traced card
// still holds it. A WPO hook that leaves the surface's plane needs a
// WPO-bake-to-BLAS path, not a better shear.
//
// Keep the two sides in sync: EvalGust and MaterialGetWorldPositionOffset in
// asset/Shaders/foliage_wind.hlsl are one formula written twice. If they drift,
// the RT shadow desyncs from the visible leaves silently and smoothly. Change both,
// and keep the constants (0.35 phase scale, the 0.75/0.25 harmonic split, 2.3/1.7)
// identical.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <DirectXMath.h>
#include <cmath>

namespace FoliageWind
{
    // Custom-param names a material must declare for the RT proxy to pick it up.
    // This IS the contract: the engine cannot introspect an arbitrary WPO hook,
    // so a material opts in by using these names with these semantics. They match
    // the cbuffer in asset/Shaders/foliage_wind.hlsl.
    inline constexpr const char* kParamDirection = "WindDirection"; // xyz world dir (Y ignored)
    inline constexpr const char* kParamMotion    = "WindMotion";    // x=strength y=speed z=flutter w=flutterSpeed

    struct Params
    {
        DirectX::XMFLOAT2 dirXZ{ 0.0f, 0.0f };  // normalized horizontal direction
        float             strength = 0.0f;      // meters of sway per meter of height
        float             speed    = 0.0f;      // oscillations / second

        // A material with no sway must skip the shear entirely: a near-identity matrix
        // would still change the instance bytes every frame, defeat the TLAS fingerprint
        // skip and pay a full TLAS refit on a scene that is not moving.
        bool Enabled() const
        {
            return strength > 0.0f &&
                   (dirXZ.x != 0.0f || dirXZ.y != 0.0f);
        }
    };

    // Builds Params from the two raw customParam float4s, mirroring the shader's
    // normalize and degenerate-direction fallback.
    inline Params FromRaw(const float dir[4], const float motion[4])
    {
        Params p;
        float dx = dir[0], dz = dir[2];
        float len2 = dx * dx + dz * dz;
        if (len2 < 1e-6f) { dx = 1.0f; dz = 0.0f; len2 = 1.0f; }   // shader: degenerate -> +X
        const float inv = 1.0f / std::sqrt(len2);
        p.dirXZ    = { dx * inv, dz * inv };
        p.strength = motion[0];
        p.speed    = motion[1];
        return p;
    }

    // The gust waveform. Must match foliage_wind.hlsl exactly (see the sync note at
    // the top of this file).
    //   instancePhase = (originX + originZ) * 0.35   - desyncs neighboring plants
    //   gust          = sin(t)*0.75 + sin(t*2.3 + 1.7)*0.25
    inline float EvalGust(const Params& p, float originX, float originZ, float timeSec)
    {
        const float t = timeSec * p.speed + (originX + originZ) * 0.35f;
        return std::sin(t) * 0.75f + std::sin(t * 2.3f + 1.7f) * 0.25f;
    }

    // World-space shear about the instance origin, in the engine's row-vector
    // convention (a point transforms as `pos * M`). Compose as
    // `world * ShearAboutOrigin(...)` so the shear applies after the instance
    // transform, in world space, like the shader's WPO.
    // The shader wants x' = x + kx*(y - oy), z' = z + kz*(y - oy): the linear part is
    // a shear whose second row carries kx / kz (row i holds what basis vector i
    // contributes), and the translation cancels the origin offset.
    inline DirectX::XMMATRIX ShearAboutOrigin(const Params& p,
                                              float originX, float originY, float originZ,
                                              float timeSec)
    {
        const float bend = EvalGust(p, originX, originZ, timeSec) * p.strength;
        const float kx   = p.dirXZ.x * bend;
        const float kz   = p.dirXZ.y * bend;

        return DirectX::XMMATRIX(
            1.0f,        0.0f, 0.0f,        0.0f,
            kx,          1.0f, kz,          0.0f,
            0.0f,        0.0f, 1.0f,        0.0f,
            -kx * originY, 0.0f, -kz * originY, 1.0f);
    }

    // Applies the sway to an engine world matrix. `world` is row-vector with the
    // instance origin in row 3, the point the shader reads its origin from
    // (`p.worldMatrix[3]`), so both sides pivot about the same point.
    inline DirectX::XMFLOAT4X4 ApplyToWorld(const DirectX::XMFLOAT4X4& world,
                                            const Params& p, float timeSec)
    {
        using namespace DirectX;
        const XMMATRIX shear = ShearAboutOrigin(p, world._41, world._42, world._43, timeSec);
        XMFLOAT4X4 out;
        XMStoreFloat4x4(&out, XMMatrixMultiply(XMLoadFloat4x4(&world), shear));
        return out;
    }
}
