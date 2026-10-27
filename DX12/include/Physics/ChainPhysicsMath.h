#pragma once
// Physics/ChainPhysicsMath.h - chain physics helpers: per-60-Hz coefficients made frame-rate
// independent, and the swing that aims an animated bone at its simulated child.

#include <algorithm>
#include <cmath>
#include <DirectXMath.h>

namespace ChainPhysicsMath
{
// Existing assets author coefficients at 60 Hz. Preserve that reference while
// distributing decay over elapsed simulation time, including adaptive substeps.
inline float Retention(float value, float stepSeconds)
{
    if (!(stepSeconds > 0.f)) return 1.f;
    return std::pow(std::clamp(value, 0.f, 1.f), 60.f * stepSeconds);
}

inline float Pull(float value, float stepSeconds)
{
    return 1.f - Retention(1.f - value, stepSeconds);
}

// Pose attraction changes Verlet position AND next step's implicit velocity.
// It therefore acts as a spring acceleration, not a pure first-order decay.
// A linear dt exponent would make its restoring acceleration grow with FPS.
inline float PosePull(float value, float stepSeconds)
{
    if (!(stepSeconds > 0.f)) return 0.f;
    const float referenceSteps = 60.f * stepSeconds;
    return 1.f - std::pow(1.f - std::clamp(value, 0.f, 1.f),
                         referenceSteps * referenceSteps);
}

// Apply only the swing needed to aim the animated child at the simulated child.
// Keeping the animated rotation preserves twist, including when already aligned.
inline DirectX::XMVECTOR AimAnimatedRotation(
    DirectX::FXMVECTOR childDirection, DirectX::FXMVECTOR simulatedDirection,
    DirectX::FXMVECTOR animatedRotation)
{
    using namespace DirectX;
    if (XMVectorGetX(XMVector3LengthSq(childDirection)) < 1e-12f ||
        XMVectorGetX(XMVector3LengthSq(simulatedDirection)) < 1e-12f)
        return XMQuaternionNormalize(animatedRotation);
    const XMVECTOR from = XMVector3Normalize(XMVector3Rotate(childDirection, animatedRotation));
    const XMVECTOR to = XMVector3Normalize(simulatedDirection);
    const float dot = std::clamp(XMVectorGetX(XMVector3Dot(from, to)), -1.f, 1.f);
    XMVECTOR axis = XMVector3Cross(from, to);
    const float axisLength = XMVectorGetX(XMVector3Length(axis));
    if (axisLength <= 1e-7f)
    {
        if (dot >= 0.f) return XMQuaternionNormalize(animatedRotation);
        // Antiparallel directions need a deterministic perpendicular axis.
        axis = XMVector3Normalize(XMVector3Orthogonal(from));
    }
    else
        axis = XMVectorScale(axis, 1.f / axisLength);
    const XMVECTOR swing = XMQuaternionRotationAxis(axis, std::atan2(axisLength, dot));
    return XMQuaternionNormalize(XMQuaternionMultiply(animatedRotation, swing));
}
}
