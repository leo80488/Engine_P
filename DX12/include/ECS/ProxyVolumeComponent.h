#pragma once

// ProxyVolumeComponent: the parallax-correction geometry a reflection probe projects its reflection
// ray against. Separate from ReflectionProbeComponent (HDRP model) so several probes can share one
// proxy via AttachmentRef and get identical parallax (no double image at blend seams).
//
// The proxy's world transform takes only the entity's rotation + translation; scale is ignored.
// halfExtents are metres, so the shader's worldToProxy is rigid and a ray parameter solved in proxy
// space equals the world distance (reflection_probe.hlsli SampleProbe). Editors should warn, not
// scale the box, when the entity has a non-unit scale.

#include <cstdint>
#include <DirectXMath.h>

// Values must match Reflection::kProxyShape* (ReflectionProbeTypes.h) and the PROXY_* defines in
// reflection_probe.hlsli.
enum class EProxyShape : uint32_t
{
    Infinite    = 0,   // no correction (sky / open outdoor)
    Sphere      = 1,
    OrientedBox = 2,
};

struct ProxyVolumeComponent
{
    EProxyShape shape = EProxyShape::OrientedBox;

    // Box: local-space offset of the box centre from the entity origin, and the box half-size, in
    // metres. The offset moves the box without moving the entity (entity position = capture reference
    // point).
    // TODO(reflection_probe_proxy_influence_impl_plan M5): Auto-Fit, which sets these from the room's
    // geometry; nothing fits them yet.
    DirectX::XMFLOAT3 centerOffsetLS { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 halfExtents    { 5.0f, 3.0f, 5.0f };

    // Sphere radius (metres).
    float sphereRadius = 10.0f;

    // Runtime-only edit counter, not serialized.
    // TODO(reflection_probe_proxy_influence_impl_plan M5): bump it on edit / Auto-Fit and use it as the
    // probe upload's dirty gate; nothing bumps or reads it yet.
    uint32_t revision = 0;
};
