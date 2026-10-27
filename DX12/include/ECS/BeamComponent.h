#pragma once

// BeamComponent - marks an entity rendered as a procedural-tube heavy beam. The
// Renderer pairs it with a BeamSystem slot (acquired on first sight, released on
// entity destroy) and uploads the control points + params to the GPU each frame.
// Drawing also requires:
//   - GlobalTransform (usually identity: control points are already world-space)
//   - MaterialComponent (`useCustomShader = true` + `customShaderPath`, e.g.
//     "BeamTube_InnerCore.ps.hlsl"; userBlendMode picks the opaque inner-core or
//     the additive outer-glow path)

#include "ECS/ECS.h"
#include <vector>
#include <cstdint>
#include <DirectXMath.h>

// CPU-side mirror of BeamControlPoint in shaders/BeamCommon.hlsli, same bit layout
// as BeamControlPointGPU (Graphics/BeamSystem.h); kept here so gameplay code can
// build beams without engine internals.
struct BeamControlPoint
{
    DirectX::XMFLOAT3 position { 0, 0, 0 };
    float             radius   { 0.2f };
    DirectX::XMFLOAT4 colorTint{ 1, 1, 1, 1 };
};

struct BeamComponent
{
    // Author-supplied polyline (>= 2 points). World-space when the entity transform is
    // identity (recommended); otherwise local-space, transformed by the entity world
    // matrix in the VS.
    std::vector<BeamControlPoint> controlPoints;

    // Global multiplier on each control point's radius (animate "charge-up -> fire"
    // without rebuilding the list).
    float globalRadiusScale = 1.0f;

    // Perpendicular noise wobble (set 0 for a still beam). Amplitude is in
    // metres; speed multiplies the time argument before sampling the noise.
    float wobbleAmplitude   = 0.0f;
    float wobbleSpeed       = 1.0f;

    // Renderer-internal: do not set from gameplay. BeamSystem slot id, acquired on
    // first sight, released when the component (or its entity) is destroyed.
    mutable uint32_t beamSlot = 0xFFFFFFFFu;
};
