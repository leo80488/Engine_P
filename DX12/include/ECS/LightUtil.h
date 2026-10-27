#pragma once

// LightUtil - the ONE place that converts between a light entity's orientation and the world-space
// direction its light travels.
//
// Direction is DERIVED, never stored: a light points down its entity's forward (+Z) axis, the camera's
// convention (world.r[2] is forward) and the Unity / Unreal / Godot model. LightData holds only optical
// parameters (colour, intensity, range, cone, falloff). Deriving it keeps one source of truth for
// orientation, makes parent / socket attachment (a torch on a hand bone) work with no light-specific
// case, and routes gizmo, undo/redo, serialization and animation tracks through the transform.

#include <DirectXMath.h>
#include <cmath>

namespace LightUtil
{
    // Direction the light TRAVELS (sun -> ground) = the entity's world +Z, normalised so a scaled parent
    // cannot change it. A degenerate (zero-scale / NaN) transform falls back to straight down rather than
    // pushing a NaN direction into shadow matrices and the cluster buffer.
    inline DirectX::XMVECTOR DirectionFromWorld(const DirectX::XMFLOAT4X4& world)
    {
        using namespace DirectX;
        const XMMATRIX M = XMLoadFloat4x4(&world);
        const XMVECTOR d = XMVector3Normalize(M.r[2]);
        if (XMVector3IsNaN(d) || XMVector3Equal(d, XMVectorZero()))
            return XMVectorSet(0.f, -1.f, 0.f, 0.f);
        return d;
    }

    inline DirectX::XMFLOAT3 DirectionFromWorldF(const DirectX::XMFLOAT4X4& world)
    {
        DirectX::XMFLOAT3 f;
        DirectX::XMStoreFloat3(&f, DirectionFromWorld(world));
        return f;
    }

    // Inverse: the rotation quaternion that aims +Z down `dir`. Used by the TOD sun/moon drivers (which own
    // the sun's orientation), the editor / Lua / default-scene light setup, and the light deserializer,
    // which folds the `dir=` key of older scene files into the entity's rotation.
    inline DirectX::XMFLOAT4 RotationAiming(const DirectX::XMFLOAT3& dir)
    {
        using namespace DirectX;
        XMVECTOR f = XMVector3Normalize(XMLoadFloat3(&dir));
        if (XMVector3IsNaN(f) || XMVector3Equal(f, XMVectorZero()))
            f = XMVectorSet(0.f, -1.f, 0.f, 0.f);
        // The reference up must not be parallel to f or the cross product degenerates; straight-down lights
        // (the common default) hit exactly that case.
        const XMVECTOR upRef = (std::fabs(XMVectorGetY(f)) > 0.999f)
                             ? XMVectorSet(0.f, 0.f, 1.f, 0.f)
                             : XMVectorSet(0.f, 1.f, 0.f, 0.f);
        const XMVECTOR r = XMVector3Normalize(XMVector3Cross(upRef, f));
        const XMVECTOR u = XMVector3Cross(f, r);
        // Row-vector convention: matrix rows ARE the basis vectors, so
        // (0,0,1) * M == f, which is what "forward is +Z" means here.
        const XMMATRIX M(r, u, f, XMVectorSet(0.f, 0.f, 0.f, 1.f));
        DirectX::XMFLOAT4 q;
        XMStoreFloat4(&q, XMQuaternionRotationMatrix(M));
        return q;
    }
}
