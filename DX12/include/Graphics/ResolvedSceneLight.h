#pragma once
#include <DirectXMath.h>
#include <cstdint>

enum class LightType : uint8_t;

// CPU frame payload shared by scene extraction and clustered lighting.
// Consumed synchronously by SetLights; this is not the GPU buffer ABI.
struct ResolvedSceneLight
{
    DirectX::XMFLOAT3 position; float radius;
    DirectX::XMFLOAT3 color; float intensity;
    DirectX::XMFLOAT3 direction; float spotAngle;
    LightType type;
    bool castsShadow;
    uint32_t shadowSliceIdx;
    uint32_t volLightIdx = 0xFFFFFFFFu;
    // The light's stable identity across frames (LightData::gatherSeq; 0 = unknown,
    // e.g. a preview light) and its emitter radius (LightData::sourceRadius).
    uint32_t key = 0;
    float sourceRadius = 0.f;
    // How strongly the light lights the fog in the path tracers' medium (its VolumetricLightComponent's
    // intensityScale, 0 when that opts it out; 1 without one).
    float mediumScale = 1.f;
};
