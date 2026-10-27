#pragma once

// UICanvasSystem - drives the entity-as-widget Canvas UI and produces the screen-space
// UIDrawList consumed by UIPass.
//   ResolveLayout(world, screen):
//     1. Resolve UIParent GUIDs (deferred from scene load) and build child lists.
//     2. Per UICanvas (by sortOrder): compute the scaler scale, lay out the subtree
//        (UIRect + parent UIComputedRect -> UIComputedRect, Unity RectTransform).
//   UpdateInput(world, input, screen): ResolveLayout, then
//     3. Interaction: raycast the pointer against UIInteractable targets (top-most by
//        sortKey) and update hover/press/click/focus.
//   Paint(world, drawList, dt): emit UIImage / UIText back-to-front.
//
// Per-frame contract: layout is resolved TWICE a frame - inside UpdateInput (so the hit
// test and the scripts see this frame's layout) and again right before Paint. Paint draws
// only entities that already have a UIComputedRect and scripts run in between, so a screen
// rebuilt mid-frame would otherwise draw nothing for a frame. Interaction and the capture
// publication run exactly ONCE a frame (they advance one-frame edges; twice would
// double-consume a press). Precede any new Paint call site with a ResolveLayout. The resolve
// is fingerprint-cached, so the second call is a compare in the steady state; a new layout
// input must be folded into ComputeLayoutFingerprint or it will not take effect.

#include "UI/UIDrawList.h"
// Entity / NullEntity for the focus API. World stays forward-declared: this file does
// not need its definition.
#include "ECS/ECS.h"

#include <cstdint>
#include <vector>

class World;

namespace UI
{
    struct LayoutNode;
    struct TextWrapCache;

    // Per-frame immutable input snapshot. The caller computes mouseDown / mouseUp
    // exactly once from platform state before dispatching any UI system; UI
    // consumers must not synthesize or advance those edges independently.
    struct UIInputState
    {
        Vec2  mousePos{ 0, 0 };       // canvas-space pixels (caller-translated)
        bool  mouseLeft   = false;
        bool  mouseRight  = false;
        bool  mouseMiddle = false;
        bool  mouseDown   = false;    // left-button press edge this frame
        bool  mouseUp     = false;    // left-button release edge this frame
        Vec2  wheelDelta{ 0, 0 };

        // ---- Keyboard / gamepad navigation ----
        // Edges, not levels: -1 / 0 / +1 for one step this frame, derived once by the caller like
        // the mouse edges, so no UI consumer re-derives a repeat rate or double-consumes a press.
        // navY: -1 = up (toward the previous element), +1 = down.
        // navX: -1 = left, +1 = right. Moves focus only when the focused element does not set
        //       UIInteractable::absorbNavX; scripts read it either way via Engine.GetUINav.
        int   navX = 0;
        int   navY = 0;
        bool  submit = false;         // UIConfirm edge - activates the focused element
        bool  cancel = false;         // UICancel edge - exposed, never acted on here
    };

    // Display-target dimensions shared by layout, hit testing and UIPass.
    struct UIScreen
    {
        Vec2 size{ 0, 0 };
    };

    class UICanvasSystem
    {
    public:
        struct PaintStats
        {
            uint32_t verts = 0, indices = 0, cmds = 0, effects = 0;
            uint64_t hash   = 0;   // UIDrawList::ContentHash of that part
            uint64_t paints = 0;   // Paint calls so far
        };

        // Resolve every canvas subtree into UIComputedRect. Idempotent and fingerprint-cached.
        // Call it again after anything creates or destroys UI mid-frame (scripts, the sequencer, a
        // deferred world load) and before Paint, or those nodes are not drawn that frame.
        // Returns false when there is no usable layout (no canvas, or a degenerate screen size)
        // and invalidates the rects. What that means for hover/press is the caller's business:
        // Paint reads them for tint transitions, so the pre-Paint call must not clear them.
        bool ResolveLayout(World& world, const UIScreen& screen);

        // Same, reusing the last call's screen. For callers outside the frame plumbing, such as
        // the Lua binding a UI builder calls after creating its widgets, so focus and rect queries
        // work on the tick that built them. No-op before the first call with a screen.
        bool ResolveLayout(World& world);

        // Run before gameplay so Canvas hit-testing owns the current mouse
        // gesture before scripts/controllers sample gameplay input. Resolves
        // layout first, then interacts. The once-per-frame half.
        void UpdateInput(World& world, const UIInputState& input,
                         const UIScreen& screen);

        // Paint the CURRENT layout into this frame's UIPass. Visual fields (text,
        // colour, texture) are read live here. Call ResolveLayout first.
        void Paint(World& world, UIDrawList& drawList, float dt = 0.f);

        // The game screen's layout at another screen size - a secondary view's HUD: every canvas
        // the game screen lays out, into @p out (DFS pre-order: ascending sortKey). Never writes
        // UIComputedRect, a canvas's computedScale or the hit test's targets, so the game screen
        // is untouched. In the UI paint window (glyphs bake in the paint that follows).
        void LayoutScreen(World& world, const Vec2& screenSize, std::vector<LayoutNode>& out) const;
        // Paint @p nodes (LayoutScreen's) into @p drawList as Paint paints the game screen, at its
        // UI clock (not advanced), with @p wrap holding the texts wrapped at that size.
        void PaintScreen(World& world, const std::vector<LayoutNode>& nodes, UIDrawList& drawList,
                         TextWrapCache& wrap) const;

        // Editor diagnostic overlay. Bounds are emitted into the same UIDrawList
        // as the Canvas, so they use the exact screen-space rectangles used by
        // rendering and hit testing. Game builds leave this disabled.
        void SetDebugWireframe(bool enabled, uint32_t selectedEntity = 0u)
        {
            m_debugWireframe      = enabled;
            m_debugSelectedEntity = selectedEntity;
        }

        // Convenience for callers that do not need phase separation: UpdateInput, the pre-Paint
        // ResolveLayout, then Paint.
        void Tick(World& world, const UIInputState& input,
                  const UIScreen& screen, UIDrawList& drawList, float dt = 0.f)
        {
            UpdateInput(world, input, screen);
            ResolveLayout(world, screen);
            Paint(world, drawList, dt);
        }

        // True while the pointer hovers a raycast target or an interactable
        // owns the active press gesture (so callers can suppress world input).
        bool WantsCaptureMouse() const { return m_wantsCaptureMouse; }

        // Draw-list totals of the last Paint: the Canvas drawables only, before
        // the debug wireframe. Engine.UIPaintStats reads them; the ui_canvas_stress
        // pack pins them on its fixture.
        const PaintStats& LastPaintStats() const { return m_lastPaint; }
        // Fill PaintStats::hash on every Paint (UIDrawList::ContentHash, a walk
        // over every vertex - so off unless asked: Lua Engine.UIPaintHash, the UI
        // Editor's parity pack comparing the game paint with a designer view's).
        void SetPaintHash(bool on) { m_paintHash = on; if (!on) m_lastPaint.hash = 0; }
        bool PaintHashEnabled() const { return m_paintHash; }

        // ---- Focus (process-wide) ----
        // Exactly one focus, stored process-wide like the capture flags: two canvases each holding
        // a focus would show two highlights with the pad driving only one. Static so code without a
        // UICanvasSystem reference (ScriptSystem's Lua bindings, gameplay) can read it.
        static Entity FocusedEntityGlobal();

        // Force focus to @p e (NullEntity clears it). Refused (returns false) unless @p e is a live,
        // enabled, focusable, non-culled, already laid-out interactable: a focus on something
        // invisible lets the pad drive a hidden page.
        static bool SetFocusGlobal(World& world, Entity e);

        // This frame's navigation edges, forwarded from the input snapshot by every UpdateInput.
        // NavX is published even when not acted on, so the script owning an absorbNavX element can
        // use the axis. Cancel is never acted on here: closing a screen is the screen's decision.
        static int  NavXGlobal();
        static int  NavYGlobal();
        static bool SubmitGlobal();
        static bool CancelGlobal();

        // Shared process-wide capture state used by Window's input gate. The
        // UpdateInput publishes the Canvas contribution once per frame; additional UI
        // layers may OR their contribution through MergeExternalCapture.
        static bool GlobalWantsCaptureMouse();
        static bool GlobalWantsCaptureKeyboard();
        static void SetGlobalCapture(bool wantsMouse, bool wantsKeyboard);
        static void MergeExternalCapture(bool wantsMouse, bool wantsKeyboard = false);
        static void MergeExternalCaptureMouse(bool wants)
        { MergeExternalCapture(wants, false); }
        static void MergeExternalCaptureKeyboard(bool wants)
        { MergeExternalCapture(false, wants); }

        // Sticky "a UI layer owns the keyboard" request (Lua: Engine.SetUIKeyboardCapture).
        // The Canvas itself never wants the keyboard, but a rebind screen waiting on "press any
        // key" must keep that key from also reaching gameplay. Sticky rather than a per-frame
        // MergeExternalCapture: the claim spans many frames and its scripts run after UpdateInput,
        // so a per-frame merge would be a frame late and then wiped by the next reset. UpdateInput
        // ORs it into what it publishes. Whoever sets it must clear it, or gameplay keys stay
        // silenced for the session.
        // Gates only the WM_KEYDOWN / WM_CHAR path into the Keyboard singleton; Input::Update's
        // GetAsyncKeyState sampling is unaffected (a rebind silences the action layer through
        // Input::BeginCapture).
        static void SetKeyboardCaptureRequest(bool wants);
        static bool KeyboardCaptureRequested();

    private:
        void PaintDebugWireframe(World& world, UIDrawList& drawList);
        // The Canvas drawables of the current layout, back-to-front, through the
        // shared kernel (UI/UICanvasLayout.h). Paint = this + the wireframe.
        void PaintDrawables(World& world, UIDrawList& drawList);

        // Layout half (steps 1+2; ResolveLayout calls it only when the fingerprint moved) and
        // interaction half (step 3; every UpdateInput, cache hit or not).
        void RebuildLayout(World& world, const Vec2& screenSize);
        void RunInteraction(World& world, const UIInputState& input);

        // Move focus one step along an axis to the nearest candidate whose rect centre lies in
        // that direction. Geometric on purpose: authored up/down/left/right links would be four
        // more serialized GUID fields for what the geometry already gives. Returns true when focus
        // moved.
        bool NavigateFocus(World& world, int dx, int dy);

        PaintStats m_lastPaint;
        bool       m_paintHash = false;

        bool     m_wantsCaptureMouse = false;
        bool     m_debugWireframe = false;
        uint32_t m_debugSelectedEntity = 0u;
        float m_uiTimeSec = 0.f; // UI clock - drives text jitter

        // ---- Structural layout cache ----
        // Steps 1 + 2 are a pure function of the UI component state + screen size, and a HUD rarely
        // changes shape. ResolveLayout hashes exactly those inputs into m_layoutFingerprint; on a
        // match last frame's UIComputedRects are reused and the rebuild is skipped. Step 3 (hit
        // test) still runs every frame. A fingerprint, not dirty flags: a mutation site that forgets
        // to raise a flag is an invisible stale-layout bug, while a fingerprint's worst case is one
        // rebuild that was not needed.
        uint64_t     m_layoutFingerprint = 0;
        uint32_t     m_layoutWorldEpoch  = 0;
        const World* m_layoutWorld       = nullptr;
        bool         m_layoutCacheValid  = false;
        // The one layout input that is not a component field: a UIParent whose
        // parentGuid has no entry in the GuidRegistry yet. Whatever eventually
        // registers that Guid need not touch any UI pool, so the fingerprint
        // cannot see the edge resolve. While any edge is pending, retry every
        // frame; it clears itself the frame the target appears.
        bool         m_layoutHasUnresolvedParent = false;
        // One-shot diagnostics: the first cache hit proves the cache is live; a long rebuild
        // streak means some layout input mutates every frame, making the fingerprint sweep pure
        // overhead.
        uint32_t     m_layoutRebuildStreak = 0;
        // Set whenever ResolveLayout rebuilt; UpdateInput folds it into the streak and clears it.
        // The streak counts frames, not calls: ResolveLayout runs twice a frame, and a script that
        // rewrites a UIRect every frame alternates miss/hit between the calls, which per call would
        // pin the streak at 1.
        bool         m_layoutRebuiltThisFrame = false;
        // Last screen handed to ResolveLayout, so the screen-less overload has
        // something to lay out against.
        UIScreen     m_lastScreen{};
        bool         m_haveLastScreen = false;
        bool         m_loggedCacheActive   = false;
        bool         m_loggedCacheThrash   = false;
        // Raycast targets, front-to-back. Built by the layout pass, consumed by
        // the hit test every frame, so it has to survive a cache hit.
        std::vector<uint32_t> m_raycastTargets;
    };

} // namespace UI
