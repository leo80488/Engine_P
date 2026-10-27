#pragma once

// ReflectionProbeComponent: registers a reflection probe on an entity. Each frame the renderer
// scans the World (Renderer::BuildScene_SelectProbes: slice allocation, disk-cache restore, bake
// queue) and packs the chosen probes into a StructuredBuffer<GpuShared::ReflectionProbe>
// (Renderer::BuildScene_UploadProbes). Capture point = the entity's GlobalTransform translation +
// captureOffsetLS in the entity's rotation (scale ignored).
//
// The proxy (parallax geometry) is a separate, shareable ProxyVolumeComponent referenced by
// proxyVolume (null -> Infinite proxy); the influence (weight volume) is authored inline below.

#include <cstdint>
#include <string>
#include <DirectXMath.h>

#include "ECS/GuidComponent.h"   // AttachmentRef, ECS::Guid, ECS::kInvalidGuid

// Influence (weight) volume shape. Values must match Reflection::kInflShape*
// (ReflectionProbeTypes.h) and the INFL_* defines in reflection_probe.hlsli.
enum class EInfluenceShape : uint32_t { Box = 0, Sphere = 1 };

struct ReflectionProbeComponent
{
    // Bake-cache format version: bump it whenever captured content changes. A cache with any other
    // version is re-baked instead of loaded. Version 4 content: alpha = filtered geometry coverage;
    // capture lighting with the correct sun direction, Lambert 1/PI on the direct term and the
    // clamped-cosine sky SH; each face culled on its own to the far plane (primitives and content the
    // BVH has no leaf for are captured, the probes' own primitives and shadow-only proxies are not).
    static constexpr uint32_t kCurrentBakeDataVersion = 4u;
    // Serialized-schema version; 3 = the Proxy / Influence split. A record without a version is
    // migrated by ComponentSerializers (outer / inner boxes -> influence extents + fade widths).
    static constexpr uint32_t kSerializedVersion = 3u;

    enum FLAGS : uint32_t
    {
        EMPTY        = 0,
        BAKED        = 1u << 0, // cubemap content valid
        NEEDS_REBAKE = 1u << 1, // editor / runtime requested a re-capture
    };

    // cubemapSlice while no shared-array slice is assigned (none yet, or every slot taken).
    static constexpr uint32_t kInvalidSlice = ~0u;

    // ---- Capture ----
    // TODO(reflection_probe_proxy_influence_impl_plan M4): per-probe cube resources and GUID persistence.
    // faceResolution is authored and serialized, but every probe still bakes at the shared array's face
    // size; bakedCubemap and bakeRevision are not used yet.
    uint16_t          faceResolution = 128;
    DirectX::XMFLOAT3 captureOffsetLS { 0.0f, 0.0f, 0.0f };// capture offset, entity-local (unscaled)
    ECS::Guid         bakedCubemap   = ECS::kInvalidGuid;  // asset GUID; unused, see the TODO above
    uint32_t          bakeRevision   = 0;                  // mismatch -> rebake; unused, see the TODO above

    // ---- Influence (weight volume) ----
    EInfluenceShape   influenceShape = EInfluenceShape::Box;
    DirectX::XMFLOAT3 influenceHalfExtents { 5.0f, 3.0f, 5.0f };
    DirectX::XMFLOAT3 fadePos { 0.5f, 0.5f, 0.5f };        // +X/+Y/+Z inward fade dist (m)
    DirectX::XMFLOAT3 fadeNeg { 0.5f, 0.5f, 0.5f };        // -X/-Y/-Z
    float             influenceOuterRadius = 10.0f;        // sphere
    float             influenceInnerRadius = 6.0f;

    // ---- Blending ----
    uint8_t           priority  = 0;                       // higher wins the capacity mix
    float             intensity = 1.0f;                    // radiance multiplier (independent of iblStrength)

    // ---- Editor visualisation ----
    // The debug mirror ball is a primitive Sphere mesh on this same entity (Create > Rendering >
    // Reflection Probe spawns it), hidden only through that entity's VisibilityComponent.
    // showVizSphere is the per-probe intent; the editor master switch (Debug > Scene Gizmos >
    // Reflection Probe Viz) ANDs with it, so toggling the master restores each probe's own choice.
    // The two runtime fields tell a user's Inspector edit of Visibility apart from the value the
    // editor wrote last frame; without them the master walk would overwrite the checkbox every frame.
    // Baking ignores all of this: BuildScene_SelectProbes, BuildScene_UploadProbes and
    // ReflectionProbeCapturePass never read VisibilityComponent, so a hidden probe keeps capturing.
    bool              showVizSphere    = true;   // serialized (showViz=)
    bool              vizPushedVisible = true;   // runtime: last Visible bit we wrote
    bool              vizPushValid     = false;  // runtime: vizPushedVisible is meaningful
    // Set by the deserializer for a record without showViz=: there the entity's Visibility flags are
    // the only record of a hidden ball, so the first sync seeds showVizSphere from them instead of
    // forcing the ball back on. Never set for a fresh probe or a record that carries showViz=.
    bool              vizSeedFromVisibility = false;  // runtime

    // ---- Proxy binding ----
    AttachmentRef     proxyVolume;                         // null -> Infinite proxy

    // ---- Realtime tick ----
    bool     realtime           = false;
    uint32_t tickIntervalFrames = 60;
    uint64_t lastBakedFrame     = 0;     // runtime state; not serialized

    // ---- Runtime (not serialized; cubemapSlice is renderer-assigned) ----
    uint32_t cubemapSlice = kInvalidSlice; // slice in the shared cube array (bake dest + bindless SRV key)
    uint32_t flags        = EMPTY;

    // ---- Baked-cubemap disk persistence ----
    std::string bakedCubemapPath;        // .itex path (serialized); set by Renderer::ExportBakedProbeCubemaps
    uint32_t    bakeDataVersion = 0;     // must equal kCurrentBakeDataVersion to load
    bool        cacheLoadAttempted = false; // runtime: the disk load is tried at most once

    // ---- Bake flags ----
    // The BAKED / NEEDS_REBAKE bits of flags. A finished bake sets BAKED and clears NEEDS_REBAKE
    // (Renderer::ApplyProbeBakeFeedback).
    constexpr bool IsBaked() const      { return (flags & BAKED) != 0; }
    constexpr bool NeedsRebake() const   { return (flags & NEEDS_REBAKE) != 0; }
    constexpr void SetBaked(bool v)      { if (v) flags |= BAKED;        else flags &= ~BAKED; }
    constexpr void RequestRebake()       { flags |= NEEDS_REBAKE; }
    constexpr void ClearRebakeRequest()  { flags &= ~NEEDS_REBAKE; }
};
