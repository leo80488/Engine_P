#pragma once

// World Space UI - gameplay elements anchored in the 3D world (HP bars, name plates,
// damage numbers, quest markers), drawn as billboarded quads by WorldUIBillboardPass:
//   * perspective scaling comes from the projection matrix
//   * position rides on the entity's GlobalTransform (FollowEntityComponent, a parent or
//     LocalTransform)
//   * drawn on top of the scene (DepthMode::Test is not honoured yet)
//   * never shares a render command with screen-space UI (no cmd merging, no transform
//     stack)
//
// One entity per visual (independent fade / lifetime / culling) rather than several
// content components on one entity:
//
//   Entity {
//     LocalTransform                  // position in world
//     GlobalTransform                 // populated by Transform / Follow systems
//     WorldSpaceUIComponent           // shared scale / fade / pivot / depth params
//     <Content>                       // WorldUIBar / WorldUIText / WorldUIImage / DamageNumber
//     [FollowEntityComponent]         // optional - track another entity
//   }
//
// For anything the player needs to *read* (HP, names, numbers, status icons). Decorative
// billboards (smoke, leaves, light icons) belong to the effects / decal systems.

#include "ECS/ECS.h"
#include <string>
#include <DirectXMath.h>

namespace UI
{
    enum class ScalingMode : uint8_t
    {
        ConstantPixel = 0,    // billboard keeps fixed pixel size (HUD-on-3D feel)
        ConstantWorld = 1,    // billboard keeps fixed world size - close = bigger
    };

    enum class DepthMode : uint8_t
    {
        Always       = 0,     // always on top of scene (default - HP bars / names)
        Test         = 1,     // depth-tested, hidden behind walls (not honoured by the pass yet)
        TestWithFade = 2,     // not implemented: hidden portions dimmed (per-pixel alpha)
    };

    // Shared marker for any world-space UI entity. Holds the per-instance
    // scale / fade / pivot / depth settings AND per-frame computed values
    // written by WorldSpaceUISystem (read by the render pass).
    struct WorldSpaceUIComponent
    {
        // Base size of the billboard quad.
        // ConstantPixel mode: pixel size on screen.
        // ConstantWorld mode: physical size in world metres.
        DirectX::XMFLOAT2 baseSize { 1.0f, 0.2f };

        // Pivot inside the quad: which point of the billboard sits on the entity's world
        // position. (0.5, 1.0) = bottom-centre (HP bar above head), (0.5, 0.5) = centred,
        // (0, 0) = top-left.
        DirectX::XMFLOAT2 pivot { 0.5f, 1.0f };

        // Camera-relative offset added to the entity's world position, so the UI keeps its side
        // in SCREEN SPACE however the camera orbits:
        //   .x -> along cameraRightWS  (positive = camera's right side)
        //   .y -> along world up        (positive = world +Y)
        //   .z -> along cameraForwardWS (positive = away from camera)
        // Use it for "name tag 0.3 m right of the head" rather than a horizontal offset in
        // LocalTransform / FollowEntity, which flips side when the camera moves behind the entity.
        DirectX::XMFLOAT3 screenSpaceOffset { 0.0f, 0.0f, 0.0f };

        ScalingMode scalingMode = ScalingMode::ConstantWorld;

        // Fade range (camera-space metres). Outside [fadeNear, fadeFar] alpha goes to 0 and
        // isCulled = true. 0 disables that endpoint.
        float fadeNear = 0.0f;
        float fadeFar  = 50.0f;

        // Hide entirely when the world position is behind the camera.
        bool  hideWhenBehindCamera = true;

        // Not honoured yet: WorldUIBillboardPass draws every entity as Always, on top of the
        // tonemapped scene (Test needs the scene depth as a separate PSO variant).
        DepthMode depthMode = DepthMode::Always;

        // ---- Per-frame outputs (written by WorldSpaceUISystem) ----
        // Read by the render pass every frame; editing them in the Inspector is overwritten next
        // tick.
        float computedAlpha = 1.0f;
        float computedScale = 1.0f;
        bool  isCulled      = false;
        // The same for each secondary view (index = its slot - 1), from its own camera.
        static constexpr uint32_t kViewCount = 3;
        float   viewAlpha[kViewCount] = { 0.0f, 0.0f, 0.0f };
        float   viewScale[kViewCount] = { 1.0f, 1.0f, 1.0f };
        uint8_t viewCulled = 0xFF;   // bit k: view k is culled
    };

    // ---- Content components ----
    // Each entity carries ONE content component telling the pass what to draw inside the
    // billboard quad, next to its WorldSpaceUIComponent.

    // Solid bar with bg + value-fill + border.
    struct WorldUIBarComponent
    {
        float value = 1.0f;          // 0..1
        DirectX::XMFLOAT4 fillColor       { 0.24f, 0.78f, 0.39f, 1.0f }; // green
        DirectX::XMFLOAT4 backgroundColor { 0.08f, 0.08f, 0.12f, 1.0f };
        DirectX::XMFLOAT4 borderColor     { 0.47f, 0.47f, 0.55f, 1.0f };
        float borderThick = 0.02f;   // world-space border thickness when ConstantWorld
        bool  visible     = true;
    };

    // Text rendered through the engine's SDF font atlas pages.
    struct WorldUITextComponent
    {
        std::string       text;
        // TTF path resolved through UI::FontLibrary; empty = default font. WorldSpaceUISystem::Tick
        // loads the font and bakes missing glyphs in the CPU phase; the billboard pass only does
        // bake-free lookups.
        std::string       fontPath;
        DirectX::XMFLOAT4 color { 1.0f, 1.0f, 1.0f, 1.0f };
        // Multiplier on the base text size.
        float             scale = 1.0f;
        bool              visible = true;

        // ---- Effects (Canvas UIText parity, independent toggles) ----
        bool              fxOutline = false;
        bool              fxGlow    = false;
        bool              fxShadow  = false;
        bool              fxJitter  = false;
        DirectX::XMFLOAT4 outlineColor = { 0.f, 0.f, 0.f, 1.f };
        float             outlineWidth = 2.f;
        DirectX::XMFLOAT4 glowColor    = { 1.f, 0.85f, 0.4f, 1.f };
        float             glowWidth    = 4.f;
        DirectX::XMFLOAT4 shadowColor  = { 0.f, 0.f, 0.f, 0.6f };
        float             shadowOffsetX = 2.f;
        float             shadowOffsetY = 2.f;
        float             jitterAmplitude = 2.f;
        float             jitterFrequency = 12.f;

        // ---- Custom text pixel shader (Canvas parity) ----
        // .hlsl path (same forms as UIText.customShaderPath; drag & drop OK). World shaders
        // #include "WorldTextFX.hlsli" (bindless variant of TextFX.hlsli). Empty = built-in SDF.
        std::string       customShaderPath;
        DirectX::XMFLOAT4 customParam0 = { 0.f, 0.f, 0.f, 0.f };
        DirectX::XMFLOAT4 customParam1 = { 0.f, 0.f, 0.f, 0.f };

        // Runtime: customShaderPath resolved to a RegisterTextShader slot by
        // WorldSpaceUISystem::Tick (the render pass must not mutate the global registry while the
        // graph records). Never serialized.
        uint32_t          customShaderSlotRt = 0;
    };

    struct WorldUIImageComponent
    {
        // Bindless index into the engine's t0 space2 texture table; ~0u (default) = no texture,
        // the component is hidden. A TextureSystem / IGraphicsDevice texture's handleId IS the
        // bindless index.
        uint32_t bindlessIndex = ~0u;
        DirectX::XMFLOAT4 tint { 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT2 uv0  { 0.0f, 0.0f };
        DirectX::XMFLOAT2 uv1  { 1.0f, 1.0f };
        bool     visible = true;
    };

    // Transient floating damage popup. WorldSpaceUISystem counts `lifetime` down, integrates
    // `velocity` into `currentOffset`, fades alpha in over the first 1/6 and out over the last
    // 30% of `totalLifetime`, and destroys the entity when lifetime reaches 0.
    struct DamageNumberComponent
    {
        std::string       text;        // "100" / "Crit!" / "+50"
        // TTF path resolved through UI::FontLibrary; empty = default font.
        std::string       fontPath;
        DirectX::XMFLOAT4 color         { 1.0f, 0.4f, 0.3f, 1.0f };
        float             lifetime      = 1.5f;
        float             totalLifetime = 1.5f;
        DirectX::XMFLOAT3 velocity      { 0.0f, 1.0f, 0.0f };  // world m/s
        DirectX::XMFLOAT3 currentOffset { 0.0f, 0.0f, 0.0f };

        // ---- Effects + custom shader (same semantics as WorldUIText) ----
        bool              fxOutline = false;
        bool              fxGlow    = false;
        bool              fxShadow  = false;
        bool              fxJitter  = false;
        DirectX::XMFLOAT4 outlineColor = { 0.f, 0.f, 0.f, 1.f };
        float             outlineWidth = 2.f;
        DirectX::XMFLOAT4 glowColor    = { 1.f, 0.85f, 0.4f, 1.f };
        float             glowWidth    = 4.f;
        DirectX::XMFLOAT4 shadowColor  = { 0.f, 0.f, 0.f, 0.6f };
        float             shadowOffsetX = 2.f;
        float             shadowOffsetY = 2.f;
        float             jitterAmplitude = 2.f;
        float             jitterFrequency = 12.f;
        std::string       customShaderPath;
        DirectX::XMFLOAT4 customParam0 = { 0.f, 0.f, 0.f, 0.f };
        DirectX::XMFLOAT4 customParam1 = { 0.f, 0.f, 0.f, 0.f };
        uint32_t          customShaderSlotRt = 0; // runtime (Tick-resolved)
    };

} // namespace UI
