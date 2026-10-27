#pragma once
// The mapping layer's math, all in FaceMath.h's convention (QMul(a, b) = "a, then b"; row
// vectors). Header-only, DirectXMath only: tests/unit/face_map_test.cpp pins every formula here.
//
//   calibration   c' = sat((c - neutral) / max(eps, max - neutral)); q_rel = QMul(q, Inv(q_neutral))
//   mirror        Left <-> Right channels; q -> (x, -y, -z, w) (a reflection across R: pitch kept,
//                 yaw and roll negated); eyes swap sides and negate yaw. The one mirror point.
//   head          q_rel -> yaw / pitch / roll -> gain -> limits -> q_t; chain bone k gets
//                 Slerp(I, q_t, w_k) with the weights summing to 1, so the chain adds up to q_t
//   model space   D = QMul(QMul(Inv(qB), q_face), qB): qB maps face axes to model axes (B's rows)
//   parent space  S = QMul(QMul(Pw, D), Inv(Pw)); the bone's new local rotation is QMul(base, S)

#include "FaceTracking/FaceChannels.h"
#include "FaceTracking/FaceFrame.h"
#include "FaceTracking/FaceMath.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace FaceTracking::FacePose {

using DirectX::FXMVECTOR;
using DirectX::XMVECTOR;

inline XMVECTOR Identity() { return DirectX::XMQuaternionIdentity(); }
inline XMVECTOR Inv(FXMVECTOR q) { return DirectX::XMQuaternionInverse(q); }
inline XMVECTOR Load(const DirectX::XMFLOAT4& q) { return DirectX::XMLoadFloat4(&q); }
inline DirectX::XMFLOAT4 Store(FXMVECTOR q)
{
    DirectX::XMFLOAT4 out;
    DirectX::XMStoreFloat4(&out, q);
    return out;
}

// ==== Calibration ====
struct Calibration
{
    float neutral[kMaxFaceChannels] = {};
    float maximum[kMaxFaceChannels];
    DirectX::XMFLOAT4 headNeutral = {0, 0, 0, 1};
    Calibration() { std::fill(std::begin(maximum), std::end(maximum), 1.0f); }
};

inline float CalibrateChannel(float c, float neutral, float maximum)
{
    const float span = std::max(1e-4f, maximum - neutral);
    return std::clamp((c - neutral) / span, 0.0f, 1.0f);
}

// ARKit channels go through neutral / max; extras pass unchanged (their range is the source's).
inline void Calibrate(FaceFrame& f, const Calibration& c)
{
    for (int ch = 0; ch < kArkitChannelCount; ++ch)
        if (f.Has(ch)) f.values[ch] = CalibrateChannel(f.values[ch], c.neutral[ch], c.maximum[ch]);
    if (f.flags & kFaceHasHead) f.headRot = Store(DirectX::XMQuaternionNormalize(FaceMath::QMul(Load(f.headRot), Inv(Load(c.headNeutral)))));
}

// ==== Mirror ====
// The ARKit channel on the other side ("...Left" <-> "...Right"); itself when it has no side.
inline int MirrorChannel(int ch)
{
    if (ch < 0 || ch >= kArkitChannelCount) return ch;
    const std::string_view n = kArkitChannels[ch].name;
    constexpr std::string_view kL = "Left", kR = "Right";
    std::array<char, 40> buf{};
    std::string_view other;
    if (n.size() > kL.size() && n.substr(n.size() - kL.size()) == kL)
    {
        const std::string_view stem = n.substr(0, n.size() - kL.size());
        if (stem.size() + kR.size() >= buf.size()) return ch;
        std::copy(stem.begin(), stem.end(), buf.begin());
        std::copy(kR.begin(), kR.end(), buf.begin() + stem.size());
        other = std::string_view(buf.data(), stem.size() + kR.size());
    }
    else if (n.size() > kR.size() && n.substr(n.size() - kR.size()) == kR)
    {
        const std::string_view stem = n.substr(0, n.size() - kR.size());
        if (stem.size() + kL.size() >= buf.size()) return ch;
        std::copy(stem.begin(), stem.end(), buf.begin());
        std::copy(kL.begin(), kL.end(), buf.begin() + stem.size());
        other = std::string_view(buf.data(), stem.size() + kL.size());
    }
    else
        return ch;
    const int m = FindArkitChannel(other);
    return m >= 0 ? m : ch;
}

inline DirectX::XMFLOAT4 MirrorRotation(const DirectX::XMFLOAT4& q) { return {q.x, -q.y, -q.z, q.w}; }

inline void Mirror(FaceFrame& f)
{
    FaceFrame src = f;
    for (int ch = 0; ch < kArkitChannelCount; ++ch)
    {
        const int m = MirrorChannel(ch);
        const bool had = src.Has(m);
        f.present[ch >> 6] &= ~(1ull << (ch & 63));
        if (had)
        {
            f.values[ch] = src.values[m];
            f.present[ch >> 6] |= 1ull << (ch & 63);
        }
    }
    if (f.flags & kFaceHasHead) f.headRot = MirrorRotation(src.headRot);
    if (f.flags & kFaceHasHeadPos) f.headPos.x = -src.headPos.x;
    if (f.flags & kFaceHasEyes)
        for (int side = 0; side < 2; ++side)
        {
            f.eyeYawPitch[side][0] = -src.eyeYawPitch[1 - side][0];
            f.eyeYawPitch[side][1] = src.eyeYawPitch[1 - side][1];
        }
}

// ==== Head shaping and distribution ====
struct HeadShape
{
    float gainYaw = 1.0f, gainPitch = 1.0f, gainRoll = 1.0f;
    float limitYawRad = 70.0f * FaceMath::kPi / 180.0f;
    float limitPitchRad = 45.0f * FaceMath::kPi / 180.0f;
    float limitRollRad = 35.0f * FaceMath::kPi / 180.0f;
};

inline XMVECTOR ShapeHead(FXMVECTOR qRel, const HeadShape& s)
{
    const FaceMath::YawPitchRoll ypr = FaceMath::DecomposeHead(qRel);
    return FaceMath::ComposeHead(std::clamp(ypr.yaw * s.gainYaw, -s.limitYawRad, s.limitYawRad),
                                 std::clamp(ypr.pitch * s.gainPitch, -s.limitPitchRad, s.limitPitchRad),
                                 std::clamp(ypr.roll * s.gainRoll, -s.limitRollRad, s.limitRollRad));
}

// A share of q: Slerp(I, q, w). Shares of one rotation about one axis compose exactly.
inline XMVECTOR Share(FXMVECTOR q, float w)
{
    const DirectX::XMFLOAT4 qf = Store(q);
    return Load(FaceMath::SlerpUnclamped({0, 0, 0, 1}, qf, w));
}

// Weights of the resolved chain bones renormalized to sum 1 (an unresolved role's share
// moves to the others instead of the head turning less). Returns false when none resolved.
template <size_t N>
bool NormalizeChainWeights(std::array<float, N>& w, const std::array<bool, N>& resolved, size_t count)
{
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i)
        if (resolved[i]) sum += std::max(0.0f, w[i]);
    if (!(sum > 0.0f)) return false;
    for (size_t i = 0; i < count; ++i) w[i] = resolved[i] ? std::max(0.0f, w[i]) / sum : 0.0f;
    return true;
}

// ==== Face basis ====
// Rows R, U, F: the face's right, up and forward in model space, orthonormal.
struct FaceBasis
{
    DirectX::XMFLOAT3 R = {1, 0, 0}, U = {0, 1, 0}, F = {0, 0, 1};
};

// From the rest positions of both eyes, the head and the neck (model space). False when the
// points are degenerate (coincident eyes, the eye line along the neck).
inline bool BasisFromEyes(const DirectX::XMFLOAT3& leftEye, const DirectX::XMFLOAT3& rightEye,
                          const DirectX::XMFLOAT3& head, const DirectX::XMFLOAT3& neck, FaceBasis& out)
{
    using namespace DirectX;
    const XMVECTOR r = XMVectorSubtract(XMLoadFloat3(&rightEye), XMLoadFloat3(&leftEye));
    const XMVECTOR u = XMVectorSubtract(XMLoadFloat3(&head), XMLoadFloat3(&neck));
    if (XMVectorGetX(XMVector3Length(r)) < 1e-6f || XMVectorGetX(XMVector3Length(u)) < 1e-6f) return false;
    const XMVECTOR R = XMVector3Normalize(r);
    const XMVECTOR f = XMVector3Cross(R, XMVector3Normalize(u)); // F = R x U in the engine's space
    if (XMVectorGetX(XMVector3Length(f)) < 1e-4f) return false;
    const XMVECTOR F = XMVector3Normalize(f);
    const XMVECTOR U = XMVector3Cross(F, R);
    XMStoreFloat3(&out.R, R);
    XMStoreFloat3(&out.U, XMVector3Normalize(U));
    XMStoreFloat3(&out.F, F);
    return true;
}

// From named axes of the head bone ("+Z", "-Y", ...) and the head bone's rest model rotation
// (the axes are the bone's LOCAL axes, like HeadTrack's).
inline bool AxisFromName(std::string_view name, DirectX::XMFLOAT3& out)
{
    if (name.size() != 2 || (name[0] != '+' && name[0] != '-')) return false;
    const float s = name[0] == '+' ? 1.0f : -1.0f;
    switch (name[1])
    {
    case 'X': case 'x': out = {s, 0, 0}; return true;
    case 'Y': case 'y': out = {0, s, 0}; return true;
    case 'Z': case 'z': out = {0, 0, s}; return true;
    default: return false;
    }
}

inline bool BasisFromAxes(std::string_view forward, std::string_view up, FXMVECTOR headRestModelRot, FaceBasis& out)
{
    using namespace DirectX;
    XMFLOAT3 fl, ul;
    if (!AxisFromName(forward, fl) || !AxisFromName(up, ul)) return false;
    const XMVECTOR F = XMVector3Normalize(XMVector3Rotate(XMLoadFloat3(&fl), headRestModelRot));
    const XMVECTOR Uc = XMVector3Normalize(XMVector3Rotate(XMLoadFloat3(&ul), headRestModelRot));
    const XMVECTOR r = XMVector3Cross(Uc, F); // R = U x F
    if (XMVectorGetX(XMVector3Length(r)) < 1e-4f) return false;
    const XMVECTOR R = XMVector3Normalize(r);
    XMStoreFloat3(&out.R, R);
    XMStoreFloat3(&out.U, XMVector3Normalize(XMVector3Cross(F, R)));
    XMStoreFloat3(&out.F, F);
    return true;
}

// The rotation taking face axes to model axes: (1,0,0) -> R, (0,1,0) -> U, (0,0,1) -> F.
inline XMVECTOR BasisRotation(const FaceBasis& b)
{
    using namespace DirectX;
    const XMMATRIX m(b.R.x, b.R.y, b.R.z, 0.0f, b.U.x, b.U.y, b.U.z, 0.0f, b.F.x, b.F.y, b.F.z, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    return XMQuaternionNormalize(XMQuaternionRotationMatrix(m));
}

// A face-space rotation expressed in model space.
inline XMVECTOR FaceToModel(FXMVECTOR qFace, FXMVECTOR qBasis)
{
    return DirectX::XMQuaternionNormalize(FaceMath::QMul(FaceMath::QMul(Inv(qBasis), qFace), qBasis));
}

// A model-space delta applied to a bone whose parent's model rotation is parentModel:
// the new local rotation is QMul(base, ParentSpace(D, parentModel)).
inline XMVECTOR ParentSpace(FXMVECTOR dModel, FXMVECTOR parentModel)
{
    return DirectX::XMQuaternionNormalize(FaceMath::QMul(FaceMath::QMul(parentModel, dModel), Inv(parentModel)));
}

} // namespace FaceTracking::FacePose
