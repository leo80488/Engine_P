#pragma once

// Ray: a world-space ray with viewport unprojection and intersection tests.
// Used by EditorLayer for object picking and drag-on-plane.

#include <DirectXMath.h>

struct RenderView;

struct Ray
{
    DirectX::XMFLOAT3 origin;
    DirectX::XMFLOAT3 direction; // unit length

    // Builds a ray from a viewport-relative pixel position: @p pixelX / @p pixelY are mouse
    // coordinates from the viewport content's top-left corner, @p vpW / @p vpH its size in pixels.
    static Ray FromViewport(float pixelX, float pixelY,
                            float vpW,    float vpH,
                            const RenderView& view);

    // Ray vs oriented box: the box is localCenter +- halfExtents in local space, placed in the
    // world by @p worldMatrix. Returns true on hit; outT = the entry parameter, or the exit when
    // the origin is inside the box (always >= 0).
    bool IntersectsAABB(const DirectX::XMFLOAT3&   localCenter,
                        const DirectX::XMFLOAT3&   halfExtents,
                        const DirectX::XMFLOAT4X4& worldMatrix,
                        float& outT) const;

    // Ray vs infinite plane through @p planePoint with normal @p planeNormal.
    // Returns true if not parallel and t >= 0; outT = ray parameter.
    bool IntersectPlane(const DirectX::XMFLOAT3& planePoint,
                        const DirectX::XMFLOAT3& planeNormal,
                        float& outT) const;

    // Point along the ray at parameter t: origin + t * direction.
    DirectX::XMFLOAT3 At(float t) const;
};
