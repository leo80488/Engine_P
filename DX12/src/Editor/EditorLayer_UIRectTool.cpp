// The Rect Tool: direct manipulation of Canvas UI rects in the main viewport (the contract is at
// RenderUIRectTool in EditorLayer.h).
//
// Not ImGuizmo: a Canvas node's position is solved every frame from its UIRect (anchors / pivot /
// size / offset), the parent rect and the canvas scale into UIComputedRect, so it needs a 2D tool
// (Unity's Rect Tool). RenderViewportGizmo returns early for every UI node (UI::IsUINode), so the
// two tools never claim one click; having no transform is not the discriminator, because
// SceneSerializer stamps LocalTransform + GlobalTransform onto every non-skinned node it loads.
// Skipping Manipulate leaves ImGuizmo's cached hit-test geometry stale: on a frame with no gizmo
// ask EditorLayer::GizmoIsOver() / GizmoIsUsingAny(), not ImGuizmo's IsOver() / IsUsingAny().
//
// Coordinate space: UIComputedRect is in viewport pixels, and the layout's viewport size is
// EditorLayer::GetViewportSize() (the docked panel's content region), so screen = image top-left +
// rect, with no scale term. That holds only while the scene texture renders at the panel's exact
// size; a viewport zoom / fit mode would have to change this mapping.
//
// What it writes: UIComputedRect is layout output and read-only. A drag is converted back into the
// layout input (UIRect::offset / size) by Editor::UIRectEdit, which inverts ComputeRect. Deltas are
// divided by canvasScale: authored values are in REFERENCE pixels and match screen pixels only at
// canvas scale 1.
//
// Click ownership: the docked panel's "##viewport" InvisibleButton takes ImGui's ActiveId on
// mouse-down, before this runs, so the caller computes otherItemActive with that surface excluded.
//
// Undo: one entry per drag gesture, not per mouse move: the UIRect blob is captured on press and
// one ComponentEditAction is pushed on release, as DrawComponentWithUndo does for an Inspector
// field.

#include "Editor/EditorLayer.h"

#include "UI/UICanvas.h"
#include "ECS/ECS.h"
#include "Resource/ComponentSerializers.h"
#include "Editor/Undo/ComponentBlob.h"
#include "Editor/Undo/ComponentEditAction.h"
#include "Editor/UIRectEdit.h"   // the drag maths (shared with the UI Editor)

#include "imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <typeindex>

namespace
{
    constexpr float kHandleR = 4.5f;   // handle half-size, screen px (drawn)
    constexpr float kGrabR   = 8.0f;   // grab radius (deliberately > drawn)
    constexpr float kSnapPx  = 6.0f;   // snap threshold to parent edges / centre

    // Body grab: clicking inside the rect moves it, but a large rect only grabs within kBodyBand of
    // its outline. A body grab suppresses the viewport's GPU pick for that click (uiRectAte in
    // HandleViewportPicking), and a selected full-screen backdrop (the title screen has one) would
    // otherwise make nothing in the 3D scene click-selectable. Small rects grab their whole interior,
    // as uGUI does.
    constexpr float kBodyBand = 28.0f;  // grabbable band inside a large rect
    constexpr float kBigRect  = 320.0f; // above this (reference px), band instead of fill

    // Handle ids are Editor::UIRectEdit::Handle: 0..3 corners (TL, TR, BR, BL), 4..7 edges
    // (T, R, B, L), 8 = body.

    ImVec2 HandlePos(const ImVec2& mn, const ImVec2& mx, int i)
    {
        const float cx = (mn.x + mx.x) * 0.5f;
        const float cy = (mn.y + mx.y) * 0.5f;
        switch (i)
        {
            case 0: return { mn.x, mn.y };
            case 1: return { mx.x, mn.y };
            case 2: return { mx.x, mx.y };
            case 3: return { mn.x, mx.y };
            case 4: return { cx,   mn.y };
            case 5: return { mx.x, cy   };
            case 6: return { cx,   mx.y };
            case 7: return { mn.x, cy   };
            default: return { cx, cy };
        }
    }

    ImGuiMouseCursor CursorFor(int handle)
    {
        switch (handle)
        {
            case 0: case 2: return ImGuiMouseCursor_ResizeNWSE;
            case 1: case 3: return ImGuiMouseCursor_ResizeNESW;
            case 4: case 6: return ImGuiMouseCursor_ResizeNS;
            case 5: case 7: return ImGuiMouseCursor_ResizeEW;
            default:        return ImGuiMouseCursor_ResizeAll;
        }
    }

    // Nudges `d` so that one of `self[i]` lands on one of `target[j]`. The first match wins (left
    // edge, then centre, then right edge), so a rect near a corner does not oscillate between two
    // equally close targets.
    void ApplySnap(float& d, const float (&self)[3], const float (&target)[3])
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (std::fabs(self[i] - target[j]) <= kSnapPx)
                {
                    d += target[j] - self[i];
                    return;
                }
    }
}

// Commits a gesture's accumulated edit as ONE undo entry. Called from the release path and from
// every abandon path: a drag that ends because the entity went away has already written to
// UIRect, and dropping the entry would leave an applied edit that Ctrl+Z cannot reach and that
// never marked the scene dirty. A no-op when the blob matches the press-time one.
void EditorLayer::CommitUIRectDrag()
{
    const Entity e = m_uiRectDrag.entity;
    if (!m_uiRectDrag.beforeBlob.empty() && UndoEnabled()
        && m_world && e != NullEntity && m_world->IsAlive(e))
    {
        const std::string after = Editor::Undo::SerializeComponentBlob(
            GetComponentRegistry(), *m_world, e, std::type_index(typeid(UI::UIRect)));
        if (!after.empty() && after != m_uiRectDrag.beforeBlob)
        {
            PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(
                m_world->MakeHandle(e), std::type_index(typeid(UI::UIRect)),
                m_uiRectDrag.beforeBlob, after, "UI Rect"));
        }
    }
    m_uiRectDrag = {};
}

void EditorLayer::RenderUIRectTool(ImVec2 imageTopLeft, bool otherItemActive)
{
    // Cleared on every early-out below, then re-armed at the hit test: a stale true would keep
    // suppressing the game Canvas's mouse for the rest of the run.
    m_uiRectToolClaimsMouse = false;

    if (GetPlayState() != ViewportPlayState::Stopped)
    {
        m_uiRectDrag = {};
        return;
    }

    if (!m_world || m_selectedEntity == NullEntity || !m_world->IsAlive(m_selectedEntity))
    {
        CommitUIRectDrag();
        return;
    }

    auto* rectPool = m_world->GetPool<UI::UIRect>();
    auto* crPool   = m_world->GetPool<UI::UIComputedRect>();
    // Commit here too: this is reachable with a live drag (a world swap can drop the pools), and a
    // latched m_uiRectDrag.active would permanently disable the undo shortcuts, which defer to it.
    if (!rectPool || !crPool) { CommitUIRectDrag(); return; }

    UI::UIRect*               rect = rectPool->Get(m_selectedEntity);
    const UI::UIComputedRect* cr   = crPool->Get(m_selectedEntity);
    // No UIRect: not a Canvas node. No valid computed rect: unreachable from any UICanvas, so there
    // is nothing to draw on.
    if (!rect || !cr || !cr->valid)
    {
        CommitUIRectDrag();
        return;
    }

    const float s = (cr->canvasScale > 0.f) ? cr->canvasScale : 1.f;

    const ImVec2 mn{ imageTopLeft.x + cr->rect.mn.x, imageTopLeft.y + cr->rect.mn.y };
    const ImVec2 mx{ imageTopLeft.x + cr->rect.mx.x, imageTopLeft.y + cr->rect.mx.y };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 kOutline = IM_COL32( 90, 170, 255, 230);
    const ImU32 kFill    = IM_COL32( 90, 170, 255, 255);
    const ImU32 kParent  = IM_COL32(150, 150, 160,  70);
    const ImU32 kPivot   = IM_COL32(255, 200,  60, 255);

    // Faint parent rect: the frame the anchors are relative to (without it a stretched child looks
    // like a point-anchored one).
    const UI::UIParent*       up  = m_world->GetComponent<UI::UIParent>(m_selectedEntity);
    const UI::UIComputedRect* pcr =
        (up && up->parent != NullEntity && m_world->IsAlive(up->parent))
            ? crPool->Get(up->parent) : nullptr;
    if (pcr && pcr->valid)
    {
        dl->AddRect({ imageTopLeft.x + pcr->rect.mn.x, imageTopLeft.y + pcr->rect.mn.y },
                    { imageTopLeft.x + pcr->rect.mx.x, imageTopLeft.y + pcr->rect.mx.y },
                    kParent, 0.0f, 0, 1.0f);
    }

    dl->AddRect(mn, mx, kOutline, 0.0f, 0, 1.5f);

    // Pivot marker: the point the rect is positioned about, which decides how a one-edge resize
    // moves the opposite edge. Clamped exactly as ComputeRect clamps it: an out-of-range authored
    // pivot must move the marker and the drag maths together, or the pinned edge drifts.
    const float pivX = std::clamp(rect->pivot.x, 0.f, 1.f);
    const float pivY = std::clamp(rect->pivot.y, 0.f, 1.f);
    const ImVec2 pivotPt{ mn.x + (mx.x - mn.x) * pivX,
                          mn.y + (mx.y - mn.y) * pivY };
    dl->AddCircle(pivotPt, 4.0f, kPivot, 12, 1.5f);
    dl->AddLine({ pivotPt.x - 7, pivotPt.y }, { pivotPt.x + 7, pivotPt.y }, kPivot, 1.0f);
    dl->AddLine({ pivotPt.x, pivotPt.y - 7 }, { pivotPt.x, pivotPt.y + 7 }, kPivot, 1.0f);

    for (int i = 0; i < 8; ++i)
    {
        const ImVec2 h = HandlePos(mn, mx, i);
        dl->AddRectFilled({ h.x - kHandleR, h.y - kHandleR },
                          { h.x + kHandleR, h.y + kHandleR }, kFill, 1.0f);
    }

    // ---- Hit test ----
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto hitTest = [&]() -> int
    {
        for (int i = 0; i < 8; ++i)
        {
            const ImVec2 h = HandlePos(mn, mx, i);
            if (std::fabs(mouse.x - h.x) <= kGrabR && std::fabs(mouse.y - h.y) <= kGrabR)
                return i;
        }
        if (mouse.x >= mn.x && mouse.x <= mx.x && mouse.y >= mn.y && mouse.y <= mx.y)
        {
            // Inside = move, except the free middle of a large rect (kBodyBand), judged per axis so a
            // wide-but-thin node such as a caption stays grabbable along its height. "Large" is in REFERENCE
            // pixels (hence /s): a property of the node, not of the viewport panel's size.
            const bool freeX = (mx.x - mn.x) / s > kBigRect
                            && mouse.x > mn.x + kBodyBand && mouse.x < mx.x - kBodyBand;
            const bool freeY = (mx.y - mn.y) / s > kBigRect
                            && mouse.y > mn.y + kBodyBand && mouse.y < mx.y - kBodyBand;
            if (!(freeX && freeY)) return 8;
        }
        return -1;
    };

    // Only while the pointer is over the scene image and no OTHER ImGui widget owns the mouse: a
    // panel slider dragged across the viewport must not start a rect drag underneath it. "Other" is
    // the caller's call (see otherItemActive in EditorLayer.h): the docked viewport's own
    // "##viewport" InvisibleButton is active for the whole press. Right button held = the camera owns
    // the mouse (fly look / ortho pan); HandleViewportPicking applies the same rule with `rightHeld`.
    const bool canStart = m_viewportHovered && !otherItemActive
                       && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const int  hover    = canStart ? hitTest() : -1;

    // Armed OR dragging: App must keep this press away from the game's Canvas. Arm on hover because
    // App reads the flag one frame late: waiting for a live drag would let the press through, and
    // grabbing a button's resize handle would also click the button.
    m_uiRectToolClaimsMouse = m_uiRectDrag.active || hover >= 0;

    if (m_uiRectDrag.active || hover >= 0)
        ImGui::SetMouseCursor(CursorFor(m_uiRectDrag.active ? m_uiRectDrag.handle : hover));

    if (!m_uiRectDrag.active && hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_uiRectDrag.active     = true;
        m_uiRectDrag.handle     = hover;
        m_uiRectDrag.entity     = m_selectedEntity;
        m_uiRectDrag.startSizeX = rect->size.x;
        m_uiRectDrag.startSizeY = rect->size.y;
        m_uiRectDrag.startOffX  = rect->offset.x;
        m_uiRectDrag.startOffY  = rect->offset.y;
        m_uiRectDrag.startMouse = mouse;
        m_uiRectDrag.startMn    = mn;   // pre-drag screen rect, for snapping
        m_uiRectDrag.startMx    = mx;
        // Captured once per gesture; released as one undo entry below.
        m_uiRectDrag.beforeBlob = Editor::Undo::SerializeComponentBlob(
            GetComponentRegistry(), *m_world, m_selectedEntity,
            std::type_index(typeid(UI::UIRect)));
    }

    if (!m_uiRectDrag.active) return;

    // Selection changed mid-drag (picking is suppressed while dragging, but a Hierarchy click can
    // still do it): end the gesture on the OLD entity rather than write its delta into the new one.
    if (m_uiRectDrag.entity != m_selectedEntity)
    {
        CommitUIRectDrag();
        return;
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        float dx = mouse.x - m_uiRectDrag.startMouse.x;
        float dy = mouse.y - m_uiRectDrag.startMouse.y;

        // Nothing to apply yet. Do not let zero fall through: the snap below would still run on the
        // press frame and could shift the rect by up to kSnapPx, so a click that never moved would nudge
        // the node and record an undo entry.
        if (dx == 0.f && dy == 0.f) return;

        // Snap the moved rect to the parent's edges and centre, in screen space (a fixed pixel
        // threshold the user can feel), and only for a translate: with one edge moving there is no
        // unambiguous feature to snap. Alt disables.
        if (m_uiRectDrag.handle == 8 && pcr && pcr->valid && !ImGui::GetIO().KeyAlt)
        {
            const float px0 = imageTopLeft.x + pcr->rect.mn.x;
            const float px1 = imageTopLeft.x + pcr->rect.mx.x;
            const float py0 = imageTopLeft.y + pcr->rect.mn.y;
            const float py1 = imageTopLeft.y + pcr->rect.mx.y;
            const float xt[3] = { px0, (px0 + px1) * 0.5f, px1 };
            const float yt[3] = { py0, (py0 + py1) * 0.5f, py1 };
            // Probe from the PRESS-TIME rect, never from `mn` / `mx`: those are last frame's UIComputedRect
            // and already include the drag so far. Adding the accumulated delta to them counts the motion
            // twice, and the feedback makes the rect stick short of the target or flip between two positions.
            const float sx[3] = { m_uiRectDrag.startMn.x + dx,
                                  (m_uiRectDrag.startMn.x + m_uiRectDrag.startMx.x) * 0.5f + dx,
                                  m_uiRectDrag.startMx.x + dx };
            const float sy[3] = { m_uiRectDrag.startMn.y + dy,
                                  (m_uiRectDrag.startMn.y + m_uiRectDrag.startMx.y) * 0.5f + dy,
                                  m_uiRectDrag.startMx.y + dy };
            ApplySnap(dx, sx, xt);
            ApplySnap(dy, sy, yt);
        }

        // Editor::UIRectEdit::ApplyHandleDrag does the maths (shared with the UI Editor's designer): the
        // screen delta / canvas scale into reference pixels, the stretch bound, Ctrl's quantise, and the
        // offset that keeps the opposite edge pinned.
        UI::UIRect start = *rect;
        start.size   = { m_uiRectDrag.startSizeX, m_uiRectDrag.startSizeY };
        start.offset = { m_uiRectDrag.startOffX, m_uiRectDrag.startOffY };
        Editor::UIRectEdit::Ctx rc;
        rc.hasParent = pcr && pcr->valid;
        if (rc.hasParent) rc.parent = pcr->rect;
        rc.scale = s;
        Editor::UIRectEdit::DragOptions o;
        o.quantise = ImGui::GetIO().KeyCtrl;
        const UI::UIRect out =
            Editor::UIRectEdit::ApplyHandleDrag(start, m_uiRectDrag.handle, UI::Vec2{ dx, dy }, rc, o);
        rect->size   = out.size;
        rect->offset = out.offset;
    }
    else
    {
        CommitUIRectDrag();   // release: one undo entry for the whole gesture
        return;
    }

    // Live read-out above the rect: the Inspector's numbers, without looking away from the drag.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.0f x %.0f  @ %.0f, %.0f",
                  rect->size.x, rect->size.y, rect->offset.x, rect->offset.y);
    dl->AddText({ mn.x, mn.y - 18.0f }, IM_COL32(255, 255, 255, 235), buf);
}

// Editor-side hit test of the game's own layout output (the GPU pick never sees Canvas UI);
// (x, y) is relative to the viewport image, as UIComputedRect is.
Entity EditorLayer::ScreenUIAt(float x, float y) const
{
    if (!m_world) return NullEntity;
    const auto* crPool = m_world->GetPool<UI::UIComputedRect>();
    if (!crPool) return NullEntity;
    const auto* imgPool = m_world->GetPool<UI::UIImage>();
    const auto* txtPool = m_world->GetPool<UI::UIText>();
    auto inside = [x, y](const UI::Rect& r) { return x >= r.mn.x && x < r.mx.x && y >= r.mn.y && y < r.mx.y; };
    Entity best    = NullEntity;
    int    bestKey = 0;
    const auto& ents = crPool->Entities();
    const auto& data = crPool->Data();
    for (std::size_t i = 0; i < ents.size(); ++i)
    {
        const UI::UIComputedRect& cr = data[i];
        const Entity e = ents[i];
        // The game's layout only: a valid, unculled rect (a stage, an off-screen canvas or a hidden
        // subtree has none), never a canvas root (the whole screen).
        if (!cr.valid || cr.culled || !m_world->IsAlive(e) || m_world->HasComponent<UI::UICanvas>(e)) continue;
        if (!inside(cr.rect) || !inside(cr.clipRect)) continue;
        const UI::UIImage* im = imgPool ? imgPool->Get(e) : nullptr;
        const UI::UIText*  tx = txtPool ? txtPool->Get(e) : nullptr;
        const bool draws = (im && im->visible) || (tx && tx->visible && !tx->text.empty());
        if (!draws) continue;
        if (best == NullEntity || cr.sortKey >= bestKey)   // ties: the later in the pool wins, as paint does
        {
            best    = e;
            bestKey = cr.sortKey;
        }
    }
    return best;
}
