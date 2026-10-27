#pragma once

// DebugDrawSystem - the single, editor-driven submission point for ALL editor
// debug visuals (collision / AABB / frustum / capsule / probe / DDGI wireframes,
// navmesh, and billboard icons). It keeps the toggle state as ONE category
// bitmask and routes every submission through one place, so the editor owns
// "what debug to show" and gameplay / rendering systems stay unaware of it
// (see docs/design/editor_debug_draw_system.md).
//
// Each editor frame (App::RenderPhase; every call is WITH_EDITOR-gated, so a
// non-editor exe draws no debug):
//   1. ApplyTo(renderer) - BEFORE Renderer::BeginFrame: push the category
//      mask down onto the renderer-owned buckets (DebugWirePass flags) that
//      the BeginFrame-time gather reads.
//   2. Renderer::BeginFrame() - clears the wire ring buffer + gathers the
//      renderer-internal wireframes (AABB / capsule / frustum / probe / DDGI /
//      decal / character capsule) for the enabled categories.
//   3. Submit(renderer, world, ctx, physics) - AFTER BeginFrame (ring slot
//      live) and BEFORE Render: emit the debug icons and the external wireframes
//      (light ranges, post-process / fog volumes, collision mesh, and what the
//      engine modules contribute: the Nav module's navmesh and soup) for the
//      enabled categories.
//
// The class itself is plain driver code (in EngineCore so every exe links it);
// the editor-only gating is at the App call sites.

#include "Editor/DebugCategory.h"

#include <DirectXMath.h>

#include <cstdint>
#include <functional>

class DebugWirePass;
class Renderer;
class World;
struct FrameContext;
namespace DX12Physics { class PhysicsSystem; }

class DebugDrawSystem
{
public:
    // ---- Debug icon kinds (data-driven registry) ----
    struct IconKindInfo { const char* name; DebugCategory category; };

    // ---- Module wireframes ----
    // What a contributor draws with, during Submit.
    struct WireContext
    {
        DebugWirePass&    wire;
        World&            world;
        DirectX::XMFLOAT3 cameraPos;     // the view camera's position
        float             maxDistance;   // the Collision overlay's range dial (0 = all)
    };
    // A wireframe an engine module draws while its category is active: the core
    // owns the toggle (DebugCategory), the module the geometry.
    struct WireContributor
    {
        DebugCategory                           category;
        std::function<void(const WireContext&)> emit;
    };

    // ---- Central toggle state ----
    // Is a WIREFRAME category active this frame? Wireframe overlays require the
    // master switch. (Icon categories like Lights/Cameras are standalone gizmos
    // gated directly by their bit: see the icon-kind registry below.)
    bool Active(DebugCategory c) const
    {
        return masterEnabled && DebugCatEnabled(categoryMask, c);
    }

    // ---- Debug icon kinds (data-driven registry) ----
    // Adding a NEW debug icon (e.g. audio / spawn point) takes two small steps and
    // NO new GPU / loading code:
    //   1. add one DebugCategory bit in DebugCategory.h
    //   2. add one row to kIconKinds[] in DebugDrawSystem.cpp:
    //        { DebugCategory::MyThing, "My Icons", 0xF0EBu, nullptr,
    //          &EmitIconsForPool<MyComponent>, 0.08f }
    //      (a Font Awesome codepoint; give glyph 0 and a .itex texturePath only
    //      when no glyph fits).
    // Both the Debug-menu toggle and the rendering then pick it up via these
    // accessors. The icon's bindless texture is resolved generically
    // (Renderer::GetGlyphIconBindless, or GetDebugIconBindless for a .itex).
    static int          IconKindCount();
    static IconKindInfo GetIconKindInfo(int index);
    // Font Awesome codepoint for icon kind `index` (0 = texture-based). The
    // editor bakes these glyphs into engine textures once (Renderer::RegisterGlyphIcon).
    static uint32_t     GetIconKindGlyph(int index);

    // ---- Module wireframes ----
    // A module's Initialize; drawn by every DebugDrawSystem's Submit after the core's.
    static void AddWireContributor(WireContributor contributor);

    // ---- Per-frame calls ----
    // Push the central state onto the renderer-owned buckets. Call BEFORE
    // Renderer::BeginFrame (whose wire-gather reads these flags).
    void ApplyTo(Renderer& renderer) const;

    // Emit the debug icons and the external wireframes (light ranges, volumes,
    // collision mesh, the module contributors') for the enabled categories. Call
    // AFTER Renderer::BeginFrame and BEFORE Renderer::Render.
    void Submit(Renderer& renderer, World& world, const FrameContext& ctx,
                DX12Physics::PhysicsSystem& physics) const;

    // ---- Central toggle state ----
    // Master switch for the wireframe overlays. Light icons are independent of
    // it (they are a standalone gizmo), gated only by the Lights category bit.
    bool     masterEnabled       = false;
    // Master switch for the standalone debug ICONS (camera/light/VFX gizmos),
    // independent of the wireframe master above; per-kind category bits still
    // apply underneath it.
    bool     iconsEnabled        = true;
    float    iconMaxDistance     = 50.0f; // camera radius in world units
    float    iconSizeScale       = 1.0f;
    // Categories enabled. Light + Camera + VFX icons ON by default so gizmos
    // show in a fresh editor; wireframe categories stay OFF (need the master).
    uint32_t categoryMask        = DebugCategory::Lights | DebugCategory::Cameras
                                 | DebugCategory::VFX;
    // 0 = unlimited collision-wireframe range; the Debug menu dials it down.
    float    collisionMaxDistance = 0.0f;
    // The editor's selection (an Entity; 0 = none), set by App each frame: the
    // collision overlay draws its collider highlighted.
    std::uint32_t selectedEntity  = 0;
};
