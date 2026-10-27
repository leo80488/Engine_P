#pragma once

// The viewport's view cube: a small cube in the corner, turned like the editor camera. Clicking a
// face selects the ortho view that looks at that face (Editor::OrthoViews, the one table); dragging
// turns the camera. The math is header-only so the face table is unit-tested against OrthoViews;
// Draw (EditorViewCube.cpp) is the ImGui half.

#include "Editor/EditorOrthoViews.h"

#include <DirectXMath.h>

#include <cmath>

struct ImDrawList;

namespace Editor::ViewCube
{
    // A camera's world basis, left-handed: right = cross(up, forward).
    struct Basis
    {
        DirectX::XMFLOAT3 right;
        DirectX::XMFLOAT3 up;
        DirectX::XMFLOAT3 forward;
    };

    // What the user did with the cube this frame.
    struct Input
    {
        int   clickedView = -1;    // an OrthoViews index: the face clicked (press and release, no drag)
        bool  hovered     = false; // the viewport must not pick or start a gizmo through it
        bool  dragging    = false;
        float dragX = 0.f, dragY = 0.f;   // pixels this frame while dragging
    };

    // The basis a view matrix (DirectXMath, row vectors) looks with: its columns.
    inline Basis BasisOfView(const DirectX::XMFLOAT4X4& view)
    {
        return { { view._11, view._21, view._31 },
                 { view._12, view._22, view._32 },
                 { view._13, view._23, view._33 } };
    }

    // Looking at a face is looking against its outward normal: the face of ortho view @p view.
    inline DirectX::XMFLOAT3 FaceNormal(int view)
    {
        const DirectX::XMFLOAT3& f = OrthoViews::kForward[view];
        return { -f.x, -f.y, -f.z };
    }

    // The ortho view whose face has outward normal @p n (an axis), or -1.
    inline int ViewForNormal(const DirectX::XMFLOAT3& n)
    {
        for (int i = 0; i < 6; ++i)
        {
            const DirectX::XMFLOAT3 f = FaceNormal(i);
            if (std::fabs(f.x - n.x) < 1e-4f && std::fabs(f.y - n.y) < 1e-4f && std::fabs(f.z - n.z) < 1e-4f)
                return i;
        }
        return -1;
    }

    // A cube point in view space: x right, y up, z depth (larger = farther).
    inline DirectX::XMFLOAT3 ToView(const Basis& b, const DirectX::XMFLOAT3& p)
    {
        return { p.x * b.right.x + p.y * b.right.y + p.z * b.right.z,
                 p.x * b.up.x + p.y * b.up.y + p.z * b.up.z,
                 p.x * b.forward.x + p.y * b.forward.y + p.z * b.forward.z };
    }

    // The editor camera's yaw / pitch (CameraControllerComponent: pitch = -asin(forward.y),
    // yaw = atan2(forward.x, forward.z)) of @p forward. Pitch stays inside +-89 degrees, the
    // controller's range, so a straight-down face does not lose the yaw.
    inline void YawPitchOf(const DirectX::XMFLOAT3& forward, float& yaw, float& pitch)
    {
        constexpr float kMaxPitch = 1.5533430f;   // 89 degrees
        const float y = forward.y < -1.f ? -1.f : (forward.y > 1.f ? 1.f : forward.y);
        pitch = -std::asin(y);
        if (pitch > kMaxPitch) pitch = kMaxPitch;
        if (pitch < -kMaxPitch) pitch = -kMaxPitch;
        yaw = std::atan2(forward.x, forward.z);
    }

    // Draws the cube in the square [x, x + size] x [y, y + size] of @p dl for a camera looking with
    // @p basis; @p activeView (an OrthoViews index, -1 = perspective) is highlighted. Hit tests the
    // mouse against the faces drawn. ImGui thread, inside the viewport window.
    Input Draw(ImDrawList* dl, float x, float y, float size, const Basis& basis, int activeView);
}
