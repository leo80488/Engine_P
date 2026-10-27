#include "Graphics/ShadowSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

// Practical log + uniform split, tight-sphere fit, texel-snap stabilization. The constants are
// tuned against drift, acne and shimmer under TAA: re-check all three when changing one.
void ShadowSystem::Update(const FrameInput& in)
{
    m_hasLight = false;

    const XMVECTOR lightDir = XMVector3Normalize(
        XMVectorSet(in.lightDir.x, in.lightDir.y, in.lightDir.z, 0.0f));
    if (XMVector3Equal(lightDir, XMVectorZero()))
        return;

    m_hasLight = true;

    // ---- Practical split (log + uniform blend) ----
    // Cascades 0..N-2 split nearZ..shadowFar. Cascade N-1, the "ultra-far" one, covers
    // shadowFar..kFarCascadeFar outside the formula, so its texel budget goes to the long-range
    // macro silhouette rather than the near detail the smaller cascades already cover.
    const float nearZ     = in.nearZ;
    const float farZ      = in.farZ;
    const float shadowFar = std::min(farZ, in.shadowDistance);
    const int   nearCount = kCascadeCount - 1;   // 3 in the current 4-cascade setup

    float splits[kCascadeCount + 1];
    splits[0] = nearZ;
    for (int i = 1; i < nearCount; ++i)
    {
        const float fi       = static_cast<float>(i) / static_cast<float>(nearCount);
        const float splitLog = nearZ * std::pow(shadowFar / nearZ, fi);
        const float splitUni = nearZ + (shadowFar - nearZ) * fi;
        splits[i] = kLambda * splitLog + (1.0f - kLambda) * splitUni;
    }
    splits[nearCount]     = shadowFar;       // boundary between the near cascades and the far cascade
    splits[kCascadeCount] = kFarCascadeFar;  // ultra-far cascade upper limit

    for (int i = 0; i < kCascadeCount; ++i)
        m_splits[i] = splits[i + 1];

    // One cross-fade width for every cascade boundary (shadow.hlsli blends over it in view-Z),
    // sized off the near cascades' span.
    m_shadowBlendRange = (shadowFar - nearZ) * 0.08f;

    // Advance once per Update: the shader's per-frame PCF rotation keys on ShadowFrameIndex().
    const uint32_t frameIdx = m_shadowFrameIndex++;

    // ---- Per-cascade ortho light view-projection ----
    for (int i = 0; i < kCascadeCount; ++i)
    {
        const float subNear = splits[i];
        const float subFar  = splits[i + 1];

        // Reversed Z (swapped near / far). This projection only extracts the sub-frustum's world-space
        // corners; the 8-corner unproject below is projection-agnostic, so ortho editor views swap in
        // the matching ortho sub-projection.
        XMMATRIX subProj = in.orthographic
            ? XMMatrixOrthographicLH(in.orthoHalfHeight * in.aspect * 2.0f,
                                     in.orthoHalfHeight * 2.0f, subFar, subNear)
            : XMMatrixPerspectiveFovLH(in.fov, in.aspect, subFar, subNear);
        XMMATRIX invVP   = XMMatrixInverse(nullptr, XMMatrixMultiply(in.view, subProj));

        static const XMVECTOR kNdcCorners[8] = {
            { -1, -1, 0, 1 }, {  1, -1, 0, 1 }, {  1,  1, 0, 1 }, { -1,  1, 0, 1 },
            { -1, -1, 1, 1 }, {  1, -1, 1, 1 }, {  1,  1, 1, 1 }, { -1,  1, 1, 1 },
        };

        XMVECTOR frustumCenter = XMVectorZero();
        XMVECTOR worldCorners[8];
        for (int c = 0; c < 8; ++c)
        {
            XMVECTOR wc = XMVector4Transform(kNdcCorners[c], invVP);
            wc = XMVectorScale(wc, 1.0f / XMVectorGetW(wc));
            worldCorners[c] = wc;
            frustumCenter   = XMVectorAdd(frustumCenter, wc);
        }
        frustumCenter = XMVectorScale(frustumCenter, 1.0f / 8.0f);

        float radius = 0.0f;
        for (int c = 0; c < 8; ++c)
        {
            const float d = XMVectorGetX(XMVector3Length(
                XMVectorSubtract(worldCorners[c], frustumCenter)));
            radius = std::max(radius, d);
        }

        const float smSize    = static_cast<float>(in.shadowMapSize);
        const float texelSize = (2.0f * radius) / smSize;
        // Exposed for bias scaling. The radius stays unquantized: the tight-sphere fit is rotation- and
        // translation-invariant, so it is stable frame to frame, and quantizing it would rescale the
        // whole cascade's texel grid each time the radius crossed a bucket boundary.
        m_texelWorld[i] = texelSize;

        const XMVECTOR lightPos = XMVectorSubtract(
            frustumCenter, XMVectorScale(lightDir, radius + kPullBack));

        XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        if (std::abs(XMVectorGetY(lightDir)) > 0.999f)
            up = XMVectorSet(1.f, 0.f, 0.f, 0.f);
        XMMATRIX lightView = XMMatrixLookAtLH(lightPos, frustumCenter, up);

        XMMATRIX lightProj = XMMatrixOrthographicLH(
            2.0f * radius, 2.0f * radius,
            2.0f * radius + kPullBack * 2.0f, 0.0f);

        XMMATRIX lightVP = XMMatrixMultiply(lightView, lightProj);

        // Texel-snap only: the shadow edge moves in whole texels as camera or sun move, which a static
        // pixel sees as stable. Do not add sub-texel jitter: on a static edge it changes the value every
        // frame with no motion vector, TAA's neighbourhood clamp rejects it, and it reads as shimmer.
        // The per-pixel Bayer PCF rotation in shadow.hlsli does the softening.
        {
            XMVECTOR shadowOrigin = XMVectorSet(0.f, 0.f, 0.f, 1.f);
            shadowOrigin = XMVector4Transform(shadowOrigin, lightVP);
            shadowOrigin = XMVectorScale(shadowOrigin, smSize * 0.5f);

            XMVECTOR rounded = XMVectorRound(shadowOrigin);
            XMVECTOR offset  = XMVectorSubtract(rounded, shadowOrigin);
            offset = XMVectorScale(offset, 2.0f / smSize);
            offset = XMVectorSetZ(offset, 0.f);
            offset = XMVectorSetW(offset, 0.f);

            lightProj.r[3] = XMVectorAdd(lightProj.r[3], offset);
            lightVP        = XMMatrixMultiply(lightView, lightProj);
        }
        (void)frameIdx;

        XMStoreFloat4x4(&m_matricesForPass[i],     lightVP);
        XMStoreFloat4x4(&m_matricesForLightCB[i],  XMMatrixTranspose(lightVP));
    }
}
