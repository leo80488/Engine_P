#pragma once
// The canonical face space and its sign conventions - the one convention of the whole face
// tracking feature, pinned by tests/unit/face_core_test.cpp - plus the quaternion
// interpolation the source history uses. Header-only, DirectXMath only, so the unit test
// needs no engine library.
//
// Axes are the SUBJECT's own anatomy: R = their right, U = up, F = where the face points
// (towards the camera when neutral). In the engine's left-handed space F = R x U, so
// R = +X, U = +Y, F = +Z. QMul(a, b) means "a, then b" (row vectors: v * a * b), which is
// XMQuaternionMultiply(a, b). With that:
//   yaw   > 0  the face turns to the subject's right           = about U by +yaw
//   pitch > 0  the head looks up                                = about R by -pitch
//   roll  > 0  the top of the head tips towards the right shoulder = about F by -roll
// Eyes use the same yaw / pitch signs; an opening jaw turns about R by +angle.
// The head composes in one fixed order, roll, then pitch, then yaw:
//   q = QMul(AxisAngle(F, -roll), QMul(AxisAngle(R, -pitch), AxisAngle(U, yaw)))
// and DecomposeHead inverts it: yaw and pitch are the azimuth and elevation of the turned F,
// roll is the twist left about F once that swing is taken out.

#include <DirectXMath.h>

#include <cmath>

namespace FaceTracking::FaceMath {

inline constexpr float kPi = 3.14159265358979323846f;

inline DirectX::XMVECTOR AxisRight() { return DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f); }
inline DirectX::XMVECTOR AxisUp() { return DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f); }
inline DirectX::XMVECTOR AxisForward() { return DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); }

// "a, then b".
inline DirectX::XMVECTOR QMul(DirectX::FXMVECTOR a, DirectX::FXMVECTOR b)
{
    return DirectX::XMQuaternionMultiply(a, b);
}

inline DirectX::XMVECTOR AxisAngle(DirectX::FXMVECTOR unitAxis, float angle)
{
    return DirectX::XMQuaternionRotationNormal(unitAxis, angle);
}

inline DirectX::XMVECTOR Rotate(DirectX::FXMVECTOR v, DirectX::FXMVECTOR q)
{
    return DirectX::XMVector3Rotate(v, q);
}

// The swing of a head or an eye: pitch, then yaw.
inline DirectX::XMVECTOR ComposeSwing(float yaw, float pitch)
{
    return QMul(AxisAngle(AxisRight(), -pitch), AxisAngle(AxisUp(), yaw));
}

inline DirectX::XMVECTOR ComposeHead(float yaw, float pitch, float roll)
{
    return DirectX::XMQuaternionNormalize(QMul(AxisAngle(AxisForward(), -roll), ComposeSwing(yaw, pitch)));
}

inline DirectX::XMVECTOR ComposeEye(float yaw, float pitch)
{
    return DirectX::XMQuaternionNormalize(ComposeSwing(yaw, pitch));
}

inline DirectX::XMVECTOR JawOpen(float angle) { return AxisAngle(AxisRight(), angle); }

inline float WrapPi(float a)
{
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

struct YawPitchRoll
{
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f; // radians
};

inline YawPitchRoll DecomposeHead(DirectX::FXMVECTOR q)
{
    DirectX::XMFLOAT3 f;
    DirectX::XMStoreFloat3(&f, Rotate(AxisForward(), q));
    YawPitchRoll out;
    out.yaw = std::atan2(f.x, f.z);
    out.pitch = std::atan2(f.y, std::sqrt(f.x * f.x + f.z * f.z));
    // q = QMul(twist, swing), so twist = QMul(q, swing^-1): a turn about F by -roll.
    const DirectX::XMVECTOR swing = ComposeSwing(out.yaw, out.pitch);
    DirectX::XMFLOAT4 t;
    DirectX::XMStoreFloat4(&t, QMul(q, DirectX::XMQuaternionConjugate(swing)));
    out.roll = WrapPi(-2.0f * std::atan2(t.z, t.w));
    return out;
}

// Geodesic interpolation that also extrapolates (t outside [0, 1]) along the same arc, on
// the short way round. Inputs need not be normalized; the result is.
inline DirectX::XMFLOAT4 SlerpUnclamped(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b, float t)
{
    using namespace DirectX;
    const XMVECTOR qa = XMQuaternionNormalize(XMLoadFloat4(&a));
    XMVECTOR qb = XMQuaternionNormalize(XMLoadFloat4(&b));
    float d = XMVectorGetX(XMQuaternionDot(qa, qb));
    if (d < 0.0f)
    {
        qb = XMVectorNegate(qb);
        d = -d;
    }
    XMVECTOR r;
    if (d > 0.9995f)
    {
        r = XMVectorLerp(qa, qb, t); // nearly the same rotation: nlerp is exact enough
    }
    else
    {
        const float omega = std::acos(d);
        const float s = std::sin(omega);
        r = XMVectorAdd(XMVectorScale(qa, std::sin((1.0f - t) * omega) / s),
                        XMVectorScale(qb, std::sin(t * omega) / s));
    }
    XMFLOAT4 out;
    XMStoreFloat4(&out, XMQuaternionNormalize(r));
    return out;
}

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

} // namespace FaceTracking::FaceMath
