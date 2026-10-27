#pragma once
// The editor viewport's six axis-locked ORTHOGRAPHIC views (EditorViewMode 1..6:
// Top, Bottom, Front, Back, Left, Right): each one's camera forward and up. The ONE
// table for both the camera the views render through
// (EditorCameraController::ApplyOrthoView) and the viewport's pan (EditorLayer).

#include <DirectXMath.h>

namespace Editor::OrthoViews
{
    inline constexpr DirectX::XMFLOAT3 kForward[6] = {
        { 0.f, -1.f,  0.f },   // Top
        { 0.f,  1.f,  0.f },   // Bottom
        { 0.f,  0.f,  1.f },   // Front (looking +Z)
        { 0.f,  0.f, -1.f },   // Back
        { 1.f,  0.f,  0.f },   // Left  (looking +X)
        {-1.f,  0.f,  0.f },   // Right (looking -X)
    };
    inline constexpr DirectX::XMFLOAT3 kUp[6] = {
        { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f },
        { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f },
        { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f },
    };
    // Screen right = cross(up, forward), left-handed - derived, never a third table.
    inline DirectX::XMFLOAT3 Right(int axis)
    {
        const DirectX::XMFLOAT3& u = kUp[axis];
        const DirectX::XMFLOAT3& f = kForward[axis];
        return { u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z, u.x * f.y - u.y * f.x };
    }
}
