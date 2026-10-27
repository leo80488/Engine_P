#include "Renderer/ShadowFrustumCompute.h"

#include "Graphics/RenderTypes.h"
#include "Graphics/ShadowSystem.h"
#include "RenderGraph/RenderPass/ShadowPass.h"

// Head of RendererDetail::LightCB (Renderer.h): lightDir, lightColor. Nothing in this file
// reads it; the cascade volumes come from ShadowSystem's matrices alone.
namespace
{
    struct LightCBLayout
    {
        float     lightDir[4];
        float     lightColor[4];
    };
}

using namespace DirectX;

bool ShadowFrustumCompute::AabbVs6Planes(const BoundingBox& bb,
                                         const XMVECTOR (&planes)[6])
{
    const XMVECTOR center  = XMLoadFloat3(&bb.Center);
    const XMVECTOR extents = XMLoadFloat3(&bb.Extents);
    for (int i = 0; i < 6; ++i)
    {
        const XMVECTOR p    = planes[i];
        const XMVECTOR absN = XMVectorAbs(p);
        const float    d    = XMVectorGetX(XMVector3Dot(p, center)) + XMVectorGetW(p);
        const float    r    = XMVectorGetX(XMVector3Dot(absN, extents));
        if (d - r > 0.f) return false;
    }
    return true;
}

bool ShadowFrustumCompute::IntersectsAny(const BoundingBox& bb) const
{
    if (!m_valid) return false;
    for (int sc = 0; sc < kCascadeCount; ++sc)
    {
        if (!m_aabb[sc].Intersects(bb)) continue;
        if (AabbVs6Planes(bb, m_planes[sc].planes)) return true;
    }
    return false;
}

bool ShadowFrustumCompute::Compute(const ShadowSystem& shadowSys)
{
    // Reading past ShadowSystem's matrix array would be silent memory garbage,
    // so pin the two counts together at compile time.
    static_assert(kCascadeCount == ShadowSystem::kCascadeCount,
                  "ShadowFrustumCompute and ShadowSystem must agree on cascade count");

    m_valid = false;
    // No sun -> no cascades. ShadowSystem publishes matrices only with a valid
    // directional light, and fills all kCascadeCount of them in the Update that sets the
    // flag.
    if (!shadowSys.HasValidLight()) return false;

    const XMFLOAT4X4* cascadeVP = shadowSys.CascadeMatricesForPass();

    for (int i = 0; i < kCascadeCount; ++i)
    {
        // The cascade's light VP already carries ShadowSystem's split, tight-sphere fit,
        // pull-back and texel snap, the ultra-far cascade included. Unprojecting the clip cube
        // recovers exactly the volume that cascade rasterizes, so no cascade math belongs in
        // this file.
        XMMATRIX lightVP    = XMLoadFloat4x4(&cascadeVP[i]);
        XMMATRIX invLightVP = XMMatrixInverse(nullptr, lightVP);
        static const XMVECTOR kClipCorners[8] = {
            {-1,-1, 0, 1}, { 1,-1, 0, 1}, {-1, 1, 0, 1}, { 1, 1, 0, 1},
            {-1,-1, 1, 1}, { 1,-1, 1, 1}, {-1, 1, 1, 1}, { 1, 1, 1, 1},
        };
        XMFLOAT3 obbPts[8];
        for (int c = 0; c < 8; ++c)
        {
            XMVECTOR wc = XMVector4Transform(kClipCorners[c], invLightVP);
            wc = XMVectorScale(wc, 1.0f / XMVectorGetW(wc));
            XMStoreFloat3(&obbPts[c], wc);
        }
        BoundingOrientedBox::CreateFromPoints(m_obb[i],  8, obbPts, sizeof(XMFLOAT3));
        BoundingBox::CreateFromPoints(        m_aabb[i], 8, obbPts, sizeof(XMFLOAT3));

        auto buildPlane = [&](int a, int b, int c, int ref) -> XMVECTOR
        {
            const XMVECTOR va = XMLoadFloat3(&obbPts[a]);
            const XMVECTOR vb = XMLoadFloat3(&obbPts[b]);
            const XMVECTOR vc = XMLoadFloat3(&obbPts[c]);
            XMVECTOR n = XMVector3Normalize(
                XMVector3Cross(XMVectorSubtract(vb, va),
                               XMVectorSubtract(vc, va)));
            const XMVECTOR vRef = XMLoadFloat3(&obbPts[ref]);
            if (XMVectorGetX(XMVector3Dot(n, XMVectorSubtract(vRef, va))) > 0.f)
                n = XMVectorNegate(n);
            const float d = -XMVectorGetX(XMVector3Dot(n, va));
            return XMVectorSetW(n, d);
        };
        m_planes[i].planes[0] = buildPlane(0, 1, 2, 4);
        m_planes[i].planes[1] = buildPlane(4, 6, 5, 0);
        m_planes[i].planes[2] = buildPlane(0, 2, 4, 1);
        m_planes[i].planes[3] = buildPlane(1, 5, 3, 0);
        m_planes[i].planes[4] = buildPlane(2, 3, 6, 0);
        m_planes[i].planes[5] = buildPlane(0, 4, 1, 2);
    }

    m_valid = true;
    return true;
}
