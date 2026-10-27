#pragma once

// Entity-as-widget Canvas UI (Bevy / Unity-uGUI style).
//
// Every UI element is a plain ECS entity composed of small components:
//   UICanvas        - root marker: render mode, scaler ref-resolution, sort order.
//   UIRect          - layout INPUT: anchors / pivot / size / offset (RectTransform).
//   UIParent        - hierarchy edge: which entity is this node's parent.
//   UIImage/UIText  - visual data (texture quad / SDF text + effects).
//   UIInteractable  - raycast target + interaction state (hover/press/click).
//   UIClipRect      - optional subtree clipping against this node's rect.
//   UIComputedRect  - layout OUTPUT: resolved rect / clip / scale + sort key.
//
// UICanvasSystem (UI/UICanvasSystem.h) consumes these once per frame:
//   resolve scale -> build hierarchy -> layout -> interaction -> emit to UIDrawList.
//
// Canvas entities emit into the shared UIDrawList / UIPass. A node is laid out
// when it has a UIRect and is reachable from a UICanvas through UIParent edges.

#include "UI/UIDrawList.h"          // Vec2, Rect, Color32, TextEffect
#include "ECS/ECS.h"               // Entity, NullEntity
#include "ECS/Guid.h"              // ECS::Guid (stable serialized parent ref)
#include "ECS/GuidRegistry.h"      // ECS::EnsureGuidOn (parent-attach helper)
#include "Resource/SystemHandles.h" // Resource::TextureHandle

#include <string>
#include <DirectXMath.h>

namespace UI
{
    // ==== Canvas root ====
    enum class CanvasRenderMode : uint32_t
    {
        ScreenSpaceOverlay = 0, // draws on top of everything in screen pixels
        // The only mode: there is no ScreenSpaceCamera / WorldSpace.
    };

    enum class CanvasScaleMode : uint32_t
    {
        ConstantPixelSize   = 0, // 1 UI unit == 1 screen pixel
        ScaleWithScreenSize = 1, // scale UI by screen vs reference resolution
    };

    struct UICanvas
    {
        CanvasRenderMode renderMode = CanvasRenderMode::ScreenSpaceOverlay;
        CanvasScaleMode  scaleMode  = CanvasScaleMode::ScaleWithScreenSize;
        Vec2  referenceResolution { 1920.f, 1080.f };
        float matchWidthOrHeight = 0.5f; // 0 = match width, 1 = match height
        int   sortOrder = 0;             // higher canvas draws on top
        // Whole-canvas visibility, the root's equivalent of UIRect::visible (a
        // canvas root has no UIRect: its rect IS the screen; Engine.SetUIVisible
        // on a root sets this). Hiding here removes the entire subtree from
        // rendering AND raycast.
        bool  visible = true;

        // Runtime (written by UICanvasSystem each frame).
        float computedScale = 1.f;
    };

    // ==== Layout input (Unity RectTransform model) ====
    // anchorMin/Max in [0,1] of the parent rect. When equal -> point anchor
    // (size = `size`, positioned by `offset`); when different -> the element
    // stretches across that anchor span and `size` is Unity-style sizeDelta:
    // positive values expand beyond the span, negative values inset it.
    struct UIRect
    {
        Vec2 anchorMin { 0.5f, 0.5f };
        Vec2 anchorMax { 0.5f, 0.5f };
        Vec2 pivot     { 0.5f, 0.5f };
        Vec2 size      { 160.f, 40.f }; // sizeDelta (px, scaled by canvas)
        Vec2 offset    { 0.f, 0.f };    // anchoredPosition (px, scaled by canvas)
        // Node/subtree visibility. Unlike the per-visual visible flags, this
        // also removes the whole subtree from rendering and raycast.
        bool visible = true;
        // Explicit sibling order within one UIParent. Equal values fall back
        // to Entity id so layout / hit-test / draw order stays deterministic.
        int  siblingIndex = 0;
    };

    // Optional RectMask-style clipping. When enabled, this node's resolved
    // rect is intersected with the inherited clip and becomes the effective
    // clip for this node and its whole subtree.
    struct UIClipRect
    {
        bool enabled = true;
    };

    // ==== Hierarchy edge ====
    struct UIParent
    {
        Entity    parent = NullEntity; // runtime resolved parent entity
        ECS::Guid parentGuid{};        // stable serialized reference (round-trips)
    };

    // ==== Visual: textured quad (fills the computed rect) ====
    struct UIImage
    {
        std::string             texturePath;            // stable identity (serialized)
        // Runtime identity of the path represented by `texture`; mirrored from
        // App's entity-keyed texture lease (never serialized or snapshot-copied).
        std::string             resolvedTexturePath;
        Resource::TextureHandle texture{};              // runtime handle cache
        uint64_t                srvGpuHandle = 0;        // runtime, re-resolved each frame
        DirectX::XMFLOAT4       color = { 1.f, 1.f, 1.f, 1.f };
        Vec2                    uv0 { 0.f, 0.f };
        Vec2                    uv1 { 1.f, 1.f };
        // UV addressing: Clamp (default, atlas sub-rects), Wrap/Mirror (tiling
        // when uv1 > 1). pointFilter = nearest sampling (pixel-art / sprites).
        UIWrapMode              wrapMode = UIWrapMode::Clamp;
        bool                    pointFilter = false;
        bool                    visible = true;

        // ---- 9-slice ----
        // Border insets in SOURCE TEXTURE PIXELS: x = left, y = top, z = right,
        // w = bottom. All zero (the default) draws one stretched quad.
        // Texture pixels rather than UV fractions: "the corner is 12 px of art" is
        // what the artist knows, and a UV border would have to be re-authored every
        // time the source is re-exported at a different size (the conversion uses
        // texWidth/texHeight). Corners keep their pixel size, edges stretch along
        // one axis only, the centre along both, so one small texture makes a panel
        // of any size without smearing its rounded corners.
        DirectX::XMFLOAT4       sliceBorder = { 0.f, 0.f, 0.f, 0.f };

        // RUNTIME: source texture dimensions, stamped by App's per-frame texture
        // lease pass (where srvGpuHandle is resolved), because the UI system has
        // no route to TextureSystem and should not grow one just for this. 0 while
        // the texture is not resident: the paint path then cannot slice and draws
        // the plain quad for that frame.
        float                   texWidth  = 0.f;
        float                   texHeight = 0.f;
    };

    // Sprite-sheet (atlas) animation. Drives a sibling UIImage's uv0/uv1 by
    // stepping through a `columns` x `rows` grid of frames. The image's texture
    // is the full sheet; this component picks the current frame's sub-rect.
    struct UISpriteAnimComponent
    {
        int   columns    = 4;     // frames per row
        int   rows       = 4;     // rows of frames
        int   frameCount = 0;     // 0 = columns*rows (skip trailing empty cells otherwise)
        float fps        = 12.f;
        bool  loop       = true;
        bool  playing    = true;
        bool  pingpong   = false; // bounce back and forth instead of wrapping

        // Runtime.
        float elapsed = 0.f;
        int   frame   = 0;
    };

    // ==== Visual: SDF text (aligned within the computed rect) ====
    struct UIText
    {
        std::string       text = "Text";
        // TTF path resolved through UI::FontLibrary (e.g. "engine/font/X.ttf").
        // Empty = engine default font. Glyphs bake on demand at first use.
        std::string       fontPath;
        DirectX::XMFLOAT4 color = { 1.f, 1.f, 1.f, 1.f };
        float             fontScale = 1.f;   // 1.0 == UI::kTextRefPx px tall text
        int               alignH = 1;        // 0 left, 1 centre, 2 right
        int               alignV = 1;        // 0 top,  1 middle, 2 bottom
        bool              visible = true;

        // Effects resolved by Font / UIPass's shared SDF text path.
        // Independent toggles: any combination is valid (outline + glow +
        // shadow + jitter can all be on at once).
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

        // Optional custom text pixel shader. Accepts the shaders/ shorthand
        // ("text_fx/TextFX_RainbowWave.ps.hlsl"), an explicit "shaders/..."
        // path, or an "asset/..." path (Asset Browser drag & drop); see
        // UI::RegisterTextShader for the normalization. The shader #includes
        // "TextFX.hlsli" for the SDF plumbing and reads customParam0/1 via
        // the effects table (customA/customB); TextRunT() gives the 0..1
        // position across the whole string for run-wide gradients. Empty =
        // built-in SDF shader. Composes with the fx* toggles (the shadow copy
        // stays built-in behind the custom main pass).
        std::string       customShaderPath;
        DirectX::XMFLOAT4 customParam0 = { 0.f, 0.f, 0.f, 0.f };
        DirectX::XMFLOAT4 customParam1 = { 0.f, 0.f, 0.f, 0.f };

        // ---- Word wrap ----
        // Maximum line width in REFERENCE pixels (the same space as UIRect::size,
        // so it scales with the canvas). 0 (the default) = no wrapping: only an
        // explicit '\n' breaks a line.
        // Reference pixels rather than a fraction of the rect because a label is
        // frequently wider or narrower than the box it sits in (padding, an icon
        // beside it), and "wrap at 600" is a number an author can reason about at
        // the reference resolution they design against.
        float             wrapWidth = 0.f;

        // Runtime wrap cache. NOT serialized, NOT reflected, reset by
        // ComponentCloner. Without it paint would rebuild the wrapped string
        // every frame for every wrapped label: an allocation per label per frame
        // in a pass kept free of per-frame allocation churn.
        std::string       wrapCacheOut;
        std::string       wrapCacheIn;
        float             wrapCacheWidthPx = -1.f;   // resolved px it was built for
        float             wrapCacheSizePx  = -1.f;
    };

    // ==== Interaction: raycast target + state ====
    struct UIInteractable
    {
        bool raycastTarget = true;
        bool disabled      = false;

        // ---- Keyboard / gamepad focus ----
        // Whether directional navigation can land on this element. Separate from
        // raycastTarget because the two are independent: a full-screen
        // click-eater behind a menu must swallow the POINTER without ever becoming
        // a nav stop, and a purely keyboard-reachable element is legitimate too.
        bool focusable = true;

        // "Horizontal nav belongs to me." Set by elements that use left/right for
        // their own value: a slider, a cycler, a stepper. UICanvasSystem then
        // leaves the X axis alone while this element is focused, so left/right
        // changes the value instead of jumping to a neighbour (otherwise every
        // attempt to adjust one slider in a list moves focus).
        bool absorbNavX = false;

        // Runtime state (written by UICanvasSystem; poll these from gameplay/Lua).
        bool hovered = false;
        bool pressed = false;
        bool clicked = false; // true for ONE frame on release-inside (or on submit)
        // True on exactly one element per frame across all canvases. Pointer hover
        // moves it, so a mouse and a pad never show two different selections.
        bool focused = false;

        // Optional Unity-Selectable-style state tinting of the sibling UIImage.
        bool              tintTransition = false;
        DirectX::XMFLOAT4 normalColor   = { 1.f, 1.f, 1.f, 1.f };
        DirectX::XMFLOAT4 hoverColor    = { 0.92f, 0.92f, 0.92f, 1.f };
        DirectX::XMFLOAT4 pressedColor  = { 0.78f, 0.78f, 0.78f, 1.f };
        DirectX::XMFLOAT4 disabledColor = { 0.5f, 0.5f, 0.5f, 0.5f };
    };

    // ==== Layout output ====
    struct UIComputedRect
    {
        Rect  rect;                // resolved screen-space rect (pixels)
        Rect  clipRect;            // effective screen-space subtree clip
        float canvasScale = 1.f;   // owning Canvas scaler resolved this frame
        int   sortKey = 0;         // canvas sortOrder major + tree order minor
        int   depth   = 0;         // tree depth (0 = canvas root)
        bool  culled  = false;     // no visible area after effective clipping
        bool  valid   = false;     // computed this frame
    };

    // RUNTIME-ONLY tag (no serializer, no reflection, never saved): this canvas is
    // laid out and painted ONLY by an off-screen consumer (the UI Editor's template
    // stage, the -ui_render harness capture). The game screen's layout skips it, so
    // its subtree gets no UIComputedRect: it is not painted, not hit-tested and
    // never takes focus. It is a layout input, so it is folded into
    // UICanvasSystem's layout fingerprint.
    struct UICanvasOffscreen
    {
        static constexpr uint32_t kUIEditor = 4;   // a UI Editor asset tab's stage (PreviewStageTag::kUIEditor)
        static constexpr uint32_t kUIRender = 5;   // -ui_render (HarnessCaptureSystem)
        uint32_t owner = 0;   // who put it there (PreviewStageTag-style owner id)
    };

    // Attach @p child under @p parent in the UI hierarchy, stamping a stable
    // GUID on the parent so the edge survives save/load. Use this (editor /
    // gameplay) instead of writing UIParent directly.
    inline void AttachUIParent(World& world, Entity child, Entity parent)
    {
        UIParent up;
        up.parent = parent;
        if (parent != NullEntity && world.IsAlive(parent))
            up.parentGuid = ECS::EnsureGuidOn(world, parent);
        world.AddComponent<UIParent>(child, up);
    }

    // Advance every UISpriteAnimComponent by @p dt and write the current
    // frame's grid sub-rect into the same entity's Canvas UIImage uv0/uv1.
    // Call once per frame BEFORE the Canvas system renders. Defined in
    // UICanvasSystem.cpp.
    void AdvanceSpriteAnimations(World& world, float dt);

} // namespace UI

// Edit/Play and Undo snapshots copy authored Canvas data but must never copy
// transient input, layout, or TextureSystem ownership. Restored images reacquire
// from texturePath on the next App UI pump.
template <>
struct ComponentCloner<UI::UICanvas>
{
    static UI::UICanvas Clone(const UI::UICanvas& src)
    {
        UI::UICanvas dst = src;
        dst.computedScale = 1.f;
        return dst;
    }
};

template <>
struct ComponentCloner<UI::UIImage>
{
    static UI::UIImage Clone(const UI::UIImage& src)
    {
        UI::UIImage dst = src;
        dst.resolvedTexturePath.clear();
        // Source dimensions are runtime too: App re-stamps them from the live
        // texture, and a stale pair would slice against the wrong source size.
        dst.texWidth  = 0.f;
        dst.texHeight = 0.f;
        dst.texture = Resource::TextureHandle{};
        dst.srvGpuHandle = 0;
        return dst;
    }
};

template <>
struct ComponentCloner<UI::UISpriteAnimComponent>
{
    static UI::UISpriteAnimComponent Clone(const UI::UISpriteAnimComponent& src)
    {
        UI::UISpriteAnimComponent dst = src;
        dst.elapsed = 0.f;
        dst.frame = 0;
        return dst;
    }
};

template <>
struct ComponentCloner<UI::UIText>
{
    static UI::UIText Clone(const UI::UIText& src)
    {
        UI::UIText dst = src;
        // Drop the wrap cache. It is derived state keyed on a resolved pixel width,
        // and an Edit/Play snapshot can be restored at a different canvas scale;
        // carrying the strings over would show text wrapped for the wrong width
        // until something touched the label again.
        dst.wrapCacheOut.clear();
        dst.wrapCacheIn.clear();
        dst.wrapCacheWidthPx = -1.f;
        dst.wrapCacheSizePx  = -1.f;
        return dst;
    }
};

template <>
struct ComponentCloner<UI::UIInteractable>
{
    static UI::UIInteractable Clone(const UI::UIInteractable& src)
    {
        UI::UIInteractable dst = src;
        dst.hovered = false;
        dst.pressed = false;
        dst.clicked = false;
        // Focus is transient input state like the other three. A snapshot that
        // carried it would restore a second focused element and put two highlights
        // on screen until the next navigation press resolved it.
        dst.focused = false;
        return dst;
    }
};

template <>
struct ComponentCloner<UI::UIComputedRect>
{
    static UI::UIComputedRect Clone(const UI::UIComputedRect&)
    {
        return {};
    }
};
