#pragma once

// The per-scene environment singleton: the sky / atmosphere / IBL backdrop switch, like
// Unity / Godot's WorldEnvironment ("Source: Skybox / Color / None"). One per world (first entity
// wins, like TODConfigComponent).
// Presence is the sentinel: when it exists the renderer never auto-spawns sky / atmosphere / ToD
// entities and `mode` is authoritative. When it is absent at load, Scene::MigrateEnvironment
// synthesises one from any AtmosphereComponent / SkyboxComponent (SceneDefaults).
// The detail components (AtmosphereComponent, SkyboxComponent, CloudComponent,
// HeightFogComponent) stay opt-in parameter carriers; this only selects which of them the
// backdrop + IBL path consumes each frame.

#include <cstdint>
#include <DirectXMath.h>

struct WorldEnvironmentComponent
{
    // Backdrop / indirect-lighting source (Unity's "Environment Lighting -> Source").
    enum class Mode : uint8_t
    {
        None                 = 0, // no backdrop, no sky IBL; bg = flatColor, ambient = a non-physical fill (usually 0)
        FlatColor            = 1, // solid-colour backdrop (flatColor), ambient = a uniform sky light
        StaticSkybox         = 2, // SkyboxComponent cubemaps -> backdrop + IBL (procedural atmosphere off)
        ProceduralAtmosphere = 3, // AtmosphereComponent Hillaire LUTs (the default)
    };
    Mode mode = Mode::ProceduralAtmosphere;

    // Backdrop fill for None / FlatColor, and the HDR scene-colour clear whenever no sky pass paints
    // the background. Linear RGB.
    DirectX::XMFLOAT3 flatColor = { 0.0f, 0.0f, 0.0f };

    // Ambient of the two modes without a sky asset, linear RGB, default 0 (no ambient floor). The two
    // modes mean different things by it (the editor's Environment panel labels them accordingly):
    //   FlatColor - the radiance of a uniform sky light: every pass gets it as the sky cube
    //               (iblStrength 1), so GI occludes and bounces it and the path tracer sees it.
    //   None      - a non-physical fill: a uniform environment's response (diffuse through the
    //               surface's diffuse weight, reflection through the split-sum) added on top of all
    //               GI, which never sees it.
    DirectX::XMFLOAT3 ambientColor     = { 0.0f, 0.0f, 0.0f };
    float             ambientIntensity = 1.0f;

    // ---- Sky light intensity ----
    // The per-scene master intensity of all sky-derived lighting, whichever source (StaticSkybox
    // cubemaps or ProceduralAtmosphere) provides the radiance.
    // Negative = not authored here: the renderer falls back to AtmosphereComponent::iblStrength, so a
    // scene that never touched the slider keeps that value. The Environment panel writes >= 0, which
    // then wins.
    // Semantics (contract in Lighting.ps.hlsl "Knob ownership"): the sky light's one scale - sky
    // diffuse and specular fill, and the sky every traced consumer sees on a miss (DDGI probe rays, so
    // DDGI's bounce of the sky; RT reflections; ReSTIR GI; the path tracer). DDGI's own scale is
    // ddgiDiffuseScale; probes are gated at bake time (per-probe intensity at runtime). There is no
    // separate diffuse-only sky weight: one read only by the raster fallback would split the sky
    // between GI and fallback, so the old `skyDif` / `skyScale` .iscene keys are read and ignored.
    float iblStrength        = -1.0f;

    // Time-of-Day opt-in, independent of the sky mode: a StaticSkybox scene may still animate a
    // tagged sun and a ProceduralAtmosphere scene may be frozen. Mirrored by the presence of a
    // "Time of Day" entity (TODConfig + TODOutput).
    bool enableTimeOfDay = false;
};
