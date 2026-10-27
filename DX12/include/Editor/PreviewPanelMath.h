#pragma once
// PreviewPanelMath - the two pieces of arithmetic every Synthetic-view asset
// panel shares (Mesh Editor, Skeleton Editor): the render size for the space a
// panel has, and the studio camera the renderer builds for its view - so a
// panel's CPU overlay lands on the picture it draws over. Editor-only.

#include "Renderer/Preview/PreviewStudioRig.h"   // StudioCameraBasis / kStudioCamera

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Editor::PreviewPanel
{
    // The render size for a box: 32-px steps, 128..1280 x 96..960, and an area
    // cap of 1280x720 (scaled down keeping the aspect). A recompile recreates
    // seven physical textures, so the reachable set stays coarse.
    inline void QuantiseBox(float availW, float availH, std::uint32_t& w, std::uint32_t& h)
    {
        float fw = availW > 0.f ? availW : 512.f;
        float fh = availH > 0.f ? availH : 384.f;
        const float cap = 1280.f * 720.f;
        if (fw * fh > cap)
        {
            const float s = std::sqrt(cap / (fw * fh));
            fw *= s;
            fh *= s;
        }
        auto q = [](float v, float lo, float hi)
        {
            const float c = std::clamp(v, lo, hi);
            return static_cast<std::uint32_t>(std::floor(c / 32.f)) * 32u;
        };
        w = std::max(128u, q(fw, 128.f, 1280.f));
        h = std::max(96u,  q(fh, 96.f, 960.f));
    }

    // The studio camera the renderer builds for this view (PreviewRenderer's
    // BuildPreviewCamera: the same basis function, the same reversed-Z).
    struct Cam
    {
        DirectX::XMMATRIX viewProj;
        DirectX::XMMATRIX view, proj;   // kept separate: ImGuizmo's input
        DirectX::XMVECTOR eye, forward, right, up;
    };
    inline Cam BuildCam(float yaw, float pitch, float dist, const float pan[3], float aspect)
    {
        using namespace DirectX;
        const Preview::StudioCamera& sc = Preview::kStudioCamera;
        const bool panned = pan[0] != 0.f || pan[1] != 0.f || pan[2] != 0.f;
        float e[3], f[3];
        Preview::StudioCameraBasis(yaw, pitch, dist, e, f, panned ? pan : nullptr);
        Cam c;
        c.eye     = XMVectorSet(e[0], e[1], e[2], 1.f);
        c.forward = XMVectorSet(f[0], f[1], f[2], 0.f);
        const XMVECTOR worldUp = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        c.right = XMVector3Normalize(XMVector3Cross(worldUp, c.forward));
        c.up    = XMVector3Cross(c.forward, c.right);
        c.view = XMMatrixLookToLH(c.eye, c.forward, worldUp);
        c.proj = XMMatrixPerspectiveFovLH(sc.fovY, aspect > 0.f ? aspect : 1.f,
                                          sc.farZ, sc.nearZ);   // reversed-Z
        c.viewProj = XMMatrixMultiply(c.view, c.proj);
        return c;
    }
}
