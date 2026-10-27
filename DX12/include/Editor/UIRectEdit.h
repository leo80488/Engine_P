#pragma once

// UIRectEdit - the maths of editing a Canvas UI rect by hand, with no ImGui and no
// World: the main viewport's Rect Tool and the UI Editor's designer both call it.
// Every input is in RESOLVED pixels of the layout the rect was drawn by - screen
// pixels for the main viewport (UIComputedRect), design pixels for the designer
// (UIViewService::LayoutDesc) - and every output is the layout INPUT, UIRect, in
// REFERENCE pixels.
//
// THE RELATION BEING INVERTED (UI::ComputeRect, s = canvas scale):
//     mn = anchorRef + offset*s - pivot*size'      size' = max(0, span + size*s)
//     mx = mn + size'
// so for a resolved delta d on one edge:
//     left  edge:  size -= d/s ;  offset += (d/s) * (1 - pivot)
//     right edge:  size += d/s ;  offset += (d/s) *      pivot
//     move:        offset += d/s
// - and the same on Y. The anchor span contributes a constant that differentiates
// away, so point-anchored and stretched rects drag by the same code.

#include "UI/UICanvas.h"   // UI::UIRect, UI::Rect, UI::Vec2

namespace Editor::UIRectEdit
{
    // 0..3 corners (TL, TR, BR, BL), 4..7 edges (T, R, B, L), 8 = the body.
    enum Handle : int { kTL = 0, kTR, kBR, kBL, kT, kR, kB, kL, kBody, kHandleCount };

    // Which of the four edges a handle moves (the body moves all four).
    struct Edges { float l, t, r, b; };
    Edges EdgesOf(int handle);

    // The handle's point on a rect: a corner, an edge midpoint, the centre.
    UI::Vec2 HandlePoint(const UI::Rect& r, int handle);

    struct Ctx
    {
        UI::Rect parent;            // the parent's resolved rect (the anchors' frame)
        bool     hasParent = true;  // false: no parent rect known (no stretch bound)
        float    scale = 1.f;       // the canvas scale the rect was laid out with
    };

    struct DragOptions
    {
        bool quantise     = false;  // Ctrl: whole reference pixels (size first, then offset)
        bool proportional = false;  // Shift: a corner keeps the start aspect ratio
    };

    // @p start dragged by @p delta resolved pixels on @p handle. The resolved
    // extent cannot pass through zero (a stretched rect's NEGATIVE size is its
    // inset, so the bound is size >= -span/s, per moved axis only); quantising
    // rounds the size before the offset is derived from it, so the pinned edge
    // stays where it was.
    UI::UIRect ApplyHandleDrag(const UI::UIRect& start, int handle, UI::Vec2 delta, const Ctx& c,
                               const DragOptions& o = {});

    // Snapping on one axis: @p self holds the PRE-DRAG features of the moving rect
    // (min edge, centre, max edge); @p d is the drag so far. When one of
    // self[i] + d lies within @p threshold of a target, @p d moves to put it
    // exactly there and that target is returned (a guide line to draw), else NaN.
    // The first match in self order (min, centre, max) wins, so a rect near a
    // corner does not oscillate between two equally close targets.
    float SnapAxis(float& d, const float (&self)[3], const float* targets, int count, float threshold);

    // Change the anchors (or the pivot) and solve offset / size so the rect stays
    // exactly where it is on screen (keep-visual: the UI Editor's anchor presets, its
    // Details fields with Keep visual on, and the designer's anchor and pivot handles).
    UI::UIRect SetAnchorsKeepRect(const UI::UIRect& r, UI::Vec2 aMin, UI::Vec2 aMax, const UI::Rect& resolved,
                                  const Ctx& c);
    UI::UIRect SetPivotKeepRect(const UI::UIRect& r, UI::Vec2 pivot, const UI::Rect& resolved, const Ctx& c);

    // The rect that would be laid out from @p r under @p c (UI::ComputeRect).
    UI::Rect Resolve(const UI::UIRect& r, const Ctx& c);
}
