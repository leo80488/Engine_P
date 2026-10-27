#pragma once

// TAAJitterState - per-frame Halton sub-pixel jitter and the previous-frame VP cache.
// Owns only the math and state: Renderer::UploadFrameData calls Advance() once per frame,
// applies the jitter to its projection and calls CommitFrame() with this frame's matrices;
// AdvanceToNextFrame() at the end of the frame rolls them to "previous" for TAA reprojection.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>

class TAAJitterState
{
public:
    // Halton low-discrepancy sequence (base 2 for X, base 3 for Y). @p index must be >= 1
    // for a good distribution. Returns a value in [0, 1).
    static float Halton(int index, int base)
    {
        float f = 1.0f, r = 0.0f;
        while (index > 0)
        {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(index % base);
            index /= base;
        }
        return r;
    }

    // Advances the jitter index when @p enabled is true; otherwise sets the jitter to (0,0).
    // Call once per frame before building the projection matrix. The jitter (GetJitterX/Y)
    // is in pixels, in [-0.5, 0.5).
    void Advance(bool enabled)
    {
        if (!enabled) { m_jitterX = 0.0f; m_jitterY = 0.0f; return; }
        const int jIdx = (m_jitterIndex % kCount) + 1;
        m_jitterX = Halton(jIdx, 2) - 0.5f;
        m_jitterY = Halton(jIdx, 3) - 0.5f;
        m_jitterIndex = (m_jitterIndex + 1) % kCount;
    }

    // Applies the current jitter to @p projBase (reversed-Z perspective projection) by
    // offsetting NDC X/Y by jitterPixels x 2 / viewport extent (@p vpW, @p vpH). Row-vector
    // convention: the offset goes into the 3rd row of the 4x4 matrix; Y is negated.
    DirectX::XMMATRIX ApplyJitterToProjection(DirectX::XMMATRIX projBase,
                                              uint32_t vpW,
                                              uint32_t vpH) const
    {
        using namespace DirectX;
        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, projBase);
        f._31 += m_jitterX * 2.0f / static_cast<float>(vpW);
        f._32 -= m_jitterY * 2.0f / static_cast<float>(vpH);
        return XMLoadFloat4x4(&f);
    }

    // Stores this frame's matrices. The unjittered VP becomes the previous unjittered VP
    // for TAA velocity reprojection; the jittered VP becomes the previous jittered VP for
    // SSR's reflection reprojection, so history is sampled on the previous frame's actual
    // jittered pixel grid (not ~1 px off). The inverse jittered VP is cached for TAA's CB.
    void CommitFrame(DirectX::XMMATRIX curViewProjNoJitter,
                     DirectX::XMMATRIX curViewProjJittered,
                     DirectX::XMMATRIX invJitteredViewProjTransposed)
    {
        DirectX::XMStoreFloat4x4(&m_currVPNoJitter,  curViewProjNoJitter);
        DirectX::XMStoreFloat4x4(&m_currVPJittered,  curViewProjJittered);
        DirectX::XMStoreFloat4x4(&m_invJitteredVP,   invJitteredViewProjTransposed);
    }

    // Call at the END of the frame (after the CBs are filled) to roll current -> previous.
    // Kept apart from CommitFrame() so Render() still samples the previous VP during Execute().
    void AdvanceToNextFrame()
    {
        // Last frame's jitter in pixels: what the previous frame's colour (the SSR scene-colour
        // pyramid) was rasterised with. Consumers that reproject through the UNJITTERED previous VP
        // and then read that pyramid add it back (custom_surface.hlsli MS_ScreenTraceReflection).
        m_prevJitterX = m_jitterX;
        m_prevJitterY = m_jitterY;
        m_prevVPNoJitter = m_currVPNoJitter;
        m_prevVPJittered = m_currVPJittered;
    }

    float GetJitterX() const { return m_jitterX; }
    float GetJitterY() const { return m_jitterY; }
    float GetPrevJitterX() const { return m_prevJitterX; }
    float GetPrevJitterY() const { return m_prevJitterY; }

    // Overrides the jitter with an externally computed value in pixels (FSR and DLSS run
    // their own Halton sequence with a length tuned to the upscale ratio). The caller owns
    // the sequence; this only stores the offset for ApplyJitterToProjection and consumers.
    void SetJitterPixelsDirect(float jx, float jy) { m_jitterX = jx; m_jitterY = jy; }

    const DirectX::XMFLOAT4X4& GetPrevViewProjNoJitter() const { return m_prevVPNoJitter; }
    const DirectX::XMFLOAT4X4& GetCurrViewProjNoJitter() const { return m_currVPNoJitter; }
    const DirectX::XMFLOAT4X4& GetPrevViewProjJittered() const { return m_prevVPJittered; }
    const DirectX::XMFLOAT4X4& GetCurrViewProjJittered() const { return m_currVPJittered; }
    const DirectX::XMFLOAT4X4& GetInvViewProj()          const { return m_invJitteredVP;  }

private:
    static constexpr int kCount = 16;

    int                  m_jitterIndex = 0;
    float                m_jitterX     = 0.0f;
    float                m_jitterY     = 0.0f;
    float                m_prevJitterX = 0.0f;
    float                m_prevJitterY = 0.0f;
    DirectX::XMFLOAT4X4  m_prevVPNoJitter{};
    DirectX::XMFLOAT4X4  m_currVPNoJitter{};
    DirectX::XMFLOAT4X4  m_prevVPJittered{};
    DirectX::XMFLOAT4X4  m_currVPJittered{};
    DirectX::XMFLOAT4X4  m_invJitteredVP{};
};
