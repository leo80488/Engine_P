#pragma once
// MathUtils.h - Euler angle <-> quaternion conversions (DirectXMath).

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <DirectXMath.h>
#include <cmath>

namespace MathUtils
{
    // Euler degrees (x = pitch, y = yaw, z = roll) -> quaternion (xyzw), through
    // XMQuaternionRotationRollPitchYaw: roll about Z first, then pitch about X, then yaw about Y.
    inline DirectX::XMFLOAT4 EulerDegreesToQuaternion(DirectX::XMFLOAT3 eulerDeg)
    {
        using namespace DirectX;
        XMVECTOR q = XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(eulerDeg.x),
            XMConvertToRadians(eulerDeg.y),
            XMConvertToRadians(eulerDeg.z));
        XMFLOAT4 result;
        XMStoreFloat4(&result, q);
        return result;
    }

    // Quaternion (xyzw) -> Euler degrees (pitch X, yaw Y, roll Z) by the standard ZYX intrinsic
    // (= XYZ extrinsic) decomposition. Not the inverse of EulerDegreesToQuaternion (another axis
    // order): RollPitchYawDegreesFromQuaternion is.
    inline DirectX::XMFLOAT3 QuaternionToEulerDegrees(DirectX::XMFLOAT4 quat)
    {
        using namespace DirectX;
        const float x = quat.x, y = quat.y, z = quat.z, w = quat.w;

        float sinrCosp = 2.f * (w * x + y * z);
        float cosrCosp = 1.f - 2.f * (x * x + y * y);
        float pitch = atan2f(sinrCosp, cosrCosp);

        float sinp = 2.f * (w * y - z * x);
        float yaw = fabsf(sinp) >= 1.f
            ? copysignf(XM_PIDIV2, sinp)
            : asinf(sinp);

        float sinyCosp = 2.f * (w * z + x * y);
        float cosyCosp = 1.f - 2.f * (y * y + z * z);
        float roll = atan2f(sinyCosp, cosyCosp);

        return XMFLOAT3{
            XMConvertToDegrees(pitch),
            XMConvertToDegrees(yaw),
            XMConvertToDegrees(roll)
        };
    }

    // The exact inverse of EulerDegreesToQuaternion, i.e. of
    // XMQuaternionRotationRollPitchYaw(x, y, z) - roll about Z first, then pitch
    // about X, then yaw about Y: the degrees that rebuild @p quat through that
    // same call (what a ColliderComponent's rotationEulerDeg means to
    // PhysicsSystem). Read back from the rotation matrix (row vectors); at
    // gimbal lock the roll is folded into the yaw. A -0 comes out as 0.
    inline DirectX::XMFLOAT3 RollPitchYawDegreesFromQuaternion(DirectX::XMFLOAT4 quat)
    {
        using namespace DirectX;
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(XMQuaternionNormalize(XMLoadFloat4(&quat))));
        const float sx = fmaxf(-1.f, fminf(1.f, -m._32));
        float x = asinf(sx), y = 0.f, z = 0.f;
        if (fabsf(sx) < 0.9999f)
        {
            y = atan2f(m._31, m._33);
            z = atan2f(m._12, m._22);
        }
        else
            y = atan2f(-m._13, m._11);
        return XMFLOAT3{ XMConvertToDegrees(x) + 0.f, XMConvertToDegrees(y) + 0.f, XMConvertToDegrees(z) + 0.f };
    }

} // namespace MathUtils
