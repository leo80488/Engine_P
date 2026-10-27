#include "Scene/Ray.h"
#include "Graphics/RenderTypes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace DirectX;

Ray Ray::FromViewport(float pixelX, float pixelY,
                      float vpW,    float vpH,
                      const RenderView& view)
{
    // Pixel -> NDC (DirectX convention: X right, Y up).
    const float ndcX =  (2.f * pixelX / vpW) - 1.f;
    const float ndcY = -(2.f * pixelY / vpH) + 1.f; // Y flipped

    XMMATRIX viewProj    = XMLoadFloat4x4(&view.viewProjMatrix);
    XMVECTOR det;
    XMMATRIX invViewProj = XMMatrixInverse(&det, viewProj);

    // Unproject both NDC depth extremes and order them by distance to the camera: the engine
    // renders reverse-Z (NDC z=1 near, z=0 far), so do not assume z=0 is near. `origin` is then
    // always the near point and `direction` points into the scene, so At(t) / IntersectsAABB
    // distances run from the eye, front to back, whatever the depth convention.
    XMVECTOR clipA = XMVectorSet(ndcX, ndcY, 0.f, 1.f);
    XMVECTOR clipB = XMVectorSet(ndcX, ndcY, 1.f, 1.f);

    XMVECTOR wA = XMVector4Transform(clipA, invViewProj);
    XMVECTOR wB = XMVector4Transform(clipB, invViewProj);

    // Perspective divide.
    wA = XMVectorScale(wA, 1.f / XMVectorGetW(wA));
    wB = XMVectorScale(wB, 1.f / XMVectorGetW(wB));

    const XMVECTOR cam = XMLoadFloat3(&view.cameraPosition);
    const float dA = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(wA, cam)));
    const float dB = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(wB, cam)));
    const XMVECTOR worldNear = (dA <= dB) ? wA : wB; // closer to eye = near plane
    const XMVECTOR worldFar  = (dA <= dB) ? wB : wA;

    Ray r;
    XMStoreFloat3(&r.origin,    worldNear);
    XMStoreFloat3(&r.direction, XMVector3Normalize(XMVectorSubtract(worldFar, worldNear)));
    return r;
}

bool Ray::IntersectsAABB(const XMFLOAT3&   localCenter,
                         const XMFLOAT3&   halfExtents,
                         const XMFLOAT4X4& worldMatrix,
                         float& outT) const
{
    // Transform ray into local (object) space so we can test an axis-aligned box.
    XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
    XMVECTOR det;
    XMMATRIX invWorld = XMMatrixInverse(&det, world);

    XMFLOAT3 lo, ld;
    XMStoreFloat3(&lo, XMVector3TransformCoord (XMLoadFloat3(&origin),    invWorld));
    XMStoreFloat3(&ld, XMVector3TransformNormal(XMLoadFloat3(&direction), invWorld));

    // Slab test.
    float tmin = -FLT_MAX;
    float tmax =  FLT_MAX;

    const float* c  = reinterpret_cast<const float*>(&localCenter);
    const float* he = reinterpret_cast<const float*>(&halfExtents);
    const float* o  = reinterpret_cast<const float*>(&lo);
    const float* d  = reinterpret_cast<const float*>(&ld);

    for (int i = 0; i < 3; ++i)
    {
        const float bmin = c[i] - he[i];
        const float bmax = c[i] + he[i];

        if (std::abs(d[i]) < 1e-8f)
        {
            if (o[i] < bmin || o[i] > bmax) return false;
        }
        else
        {
            float t1 = (bmin - o[i]) / d[i];
            float t2 = (bmax - o[i]) / d[i];
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }

    if (tmax < 0.f) return false;
    outT = (tmin >= 0.f) ? tmin : tmax;
    return true;
}

bool Ray::IntersectPlane(const XMFLOAT3& planePoint,
                         const XMFLOAT3& planeNormal,
                         float& outT) const
{
    XMVECTOR n   = XMVector3Normalize(XMLoadFloat3(&planeNormal));
    XMVECTOR dir = XMLoadFloat3(&direction);

    const float denom = XMVectorGetX(XMVector3Dot(n, dir));
    if (std::abs(denom) < 1e-6f) return false;

    XMVECTOR diff = XMVectorSubtract(XMLoadFloat3(&planePoint), XMLoadFloat3(&origin));
    outT = XMVectorGetX(XMVector3Dot(diff, n)) / denom;
    return outT >= 0.f;
}

XMFLOAT3 Ray::At(float t) const
{
    XMFLOAT3 result;
    XMStoreFloat3(&result,
        XMVectorAdd(XMLoadFloat3(&origin),
                    XMVectorScale(XMLoadFloat3(&direction), t)));
    return result;
}
