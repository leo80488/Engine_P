// UIRectEdit.cpp - see the header. The one copy of the drag maths and the keep-visual
// solve: the main viewport's Rect Tool and the UI Editor (its designer handles, anchor
// presets and Details fields) both call it, so they cannot drift apart. The Inspector's
// anchor presets (EditorLayer_ComponentEditors.cpp) have a solve of their own.
#include "Editor/UIRectEdit.h"

#include "UI/UICanvasLayout.h"   // UI::ComputeRect

#include <algorithm>
#include <cmath>
#include <limits>

namespace Editor::UIRectEdit
{
namespace
{
    constexpr Edges kEdges[kHandleCount] = {
        { 1, 1, 0, 0 },   // top-left
        { 0, 1, 1, 0 },   // top-right
        { 0, 0, 1, 1 },   // bottom-right
        { 1, 0, 0, 1 },   // bottom-left
        { 0, 1, 0, 0 },   // top
        { 0, 0, 1, 0 },   // right
        { 0, 0, 0, 1 },   // bottom
        { 1, 0, 0, 0 },   // left
        { 1, 1, 1, 1 },   // body
    };

    float Clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

    // The anchor span of @p r in @p parent, after ComputeRect's own clamp + swap -
    // a bound that disagreed with the layout it bounds would be no bound.
    UI::Vec2 AnchorSpan(const UI::UIRect& r, const UI::Rect& parent)
    {
        float aMinX = Clamp01(r.anchorMin.x), aMaxX = Clamp01(r.anchorMax.x);
        float aMinY = Clamp01(r.anchorMin.y), aMaxY = Clamp01(r.anchorMax.y);
        if (aMinX > aMaxX) std::swap(aMinX, aMaxX);
        if (aMinY > aMaxY) std::swap(aMinY, aMaxY);
        return { (aMaxX - aMinX) * (parent.mx.x - parent.mn.x), (aMaxY - aMinY) * (parent.mx.y - parent.mn.y) };
    }

    // Solve offset / size so @p r (its anchors and pivot already the new ones)
    // resolves to exactly @p visual in @p c.
    UI::UIRect KeepRect(UI::UIRect r, const UI::Rect& visual, const Ctx& c)
    {
        const float s = c.scale > 0.f ? c.scale : 1.f;
        const UI::Rect& p = c.parent;
        const UI::Vec2 psize{ p.mx.x - p.mn.x, p.mx.y - p.mn.y };
        float aMinX = Clamp01(r.anchorMin.x), aMaxX = Clamp01(r.anchorMax.x);
        float aMinY = Clamp01(r.anchorMin.y), aMaxY = Clamp01(r.anchorMax.y);
        if (aMinX > aMaxX) std::swap(aMinX, aMaxX);
        if (aMinY > aMaxY) std::swap(aMinY, aMaxY);
        const UI::Vec2 lo{ p.mn.x + aMinX * psize.x, p.mn.y + aMinY * psize.y };
        const UI::Vec2 span{ (aMaxX - aMinX) * psize.x, (aMaxY - aMinY) * psize.y };
        const UI::Vec2 pivot{ Clamp01(r.pivot.x), Clamp01(r.pivot.y) };
        const UI::Vec2 vsize{ visual.mx.x - visual.mn.x, visual.mx.y - visual.mn.y };
        const UI::Vec2 vpivot{ visual.mn.x + pivot.x * vsize.x, visual.mn.y + pivot.y * vsize.y };
        const UI::Vec2 anchorRef{ lo.x + pivot.x * span.x, lo.y + pivot.y * span.y };
        r.size   = { (vsize.x - span.x) / s, (vsize.y - span.y) / s };
        r.offset = { (vpivot.x - anchorRef.x) / s, (vpivot.y - anchorRef.y) / s };
        return r;
    }
}

Edges EdgesOf(int handle)
{
    return (handle >= 0 && handle < kHandleCount) ? kEdges[handle] : kEdges[kBody];
}

UI::Vec2 HandlePoint(const UI::Rect& r, int handle)
{
    const float cx = (r.mn.x + r.mx.x) * 0.5f, cy = (r.mn.y + r.mx.y) * 0.5f;
    switch (handle)
    {
    case kTL: return { r.mn.x, r.mn.y };
    case kTR: return { r.mx.x, r.mn.y };
    case kBR: return { r.mx.x, r.mx.y };
    case kBL: return { r.mn.x, r.mx.y };
    case kT:  return { cx, r.mn.y };
    case kR:  return { r.mx.x, cy };
    case kB:  return { cx, r.mx.y };
    case kL:  return { r.mn.x, cy };
    default:  return { cx, cy };
    }
}

UI::Rect Resolve(const UI::UIRect& r, const Ctx& c)
{
    return UI::ComputeRect(r, c.parent, c.scale > 0.f ? c.scale : 1.f);
}

UI::UIRect ApplyHandleDrag(const UI::UIRect& start, int handle, UI::Vec2 delta, const Ctx& c,
                           const DragOptions& o)
{
    const float s = c.scale > 0.f ? c.scale : 1.f;
    const Edges h = EdgesOf(handle);
    const bool resizing = handle != kBody;

    // Shift on a corner: one scale for both axes, the larger of the two the
    // mouse asks for, so the start aspect ratio holds.
    if (o.proportional && resizing && h.l + h.r > 0.f && h.t + h.b > 0.f)
    {
        const UI::Rect r0 = Resolve(start, c);
        const float w0 = r0.mx.x - r0.mn.x, h0 = r0.mx.y - r0.mn.y;
        if (w0 > 0.f && h0 > 0.f)
        {
            const float sx = (w0 + delta.x * (h.r - h.l)) / w0;
            const float sy = (h0 + delta.y * (h.b - h.t)) / h0;
            const float k  = std::fabs(sx - 1.f) >= std::fabs(sy - 1.f) ? sx : sy;
            delta.x = (k * w0 - w0) * (h.r - h.l);
            delta.y = (k * h0 - h0) * (h.b - h.t);
        }
    }

    // Resolved delta -> REFERENCE pixels: everything authored on UIRect is in
    // reference space; skipping the division feels right at 1:1 and drifts at
    // every other resolution.
    const float rdx = delta.x / s;
    const float rdy = delta.y / s;

    UI::UIRect out = start;
    float newSizeX = start.size.x + rdx * (h.r - h.l);
    float newSizeY = start.size.y + rdy * (h.b - h.t);

    // The resolved extent may not pass through zero (the handles would swap
    // sides mid-drag). Not size >= 0: a stretched rect's negative size is its
    // inset; the bound is size >= -span/s, and only on an axis the handle moves.
    UI::Vec2 span{ 0.f, 0.f };
    if (c.hasParent) span = AnchorSpan(start, c.parent);
    auto clampSize = [&]()
    {
        if (!resizing) return;
        if (h.r != h.l) newSizeX = (std::max)(newSizeX, -span.x / s);
        if (h.b != h.t) newSizeY = (std::max)(newSizeY, -span.y / s);
    };
    clampSize();
    if (o.quantise)
    {
        newSizeX = std::round(newSizeX);
        newSizeY = std::round(newSizeY);
        clampSize();
    }

    // The offset follows the FINAL size change (that is what pins the opposite
    // edge once the clamp engages); the body is a pure translate.
    const float pivX = Clamp01(start.pivot.x), pivY = Clamp01(start.pivot.y);
    float newOffX, newOffY;
    if (!resizing)
    {
        newOffX = start.offset.x + rdx;
        newOffY = start.offset.y + rdy;
    }
    else
    {
        const float dSizeX = newSizeX - start.size.x;
        const float dSizeY = newSizeY - start.size.y;
        newOffX = start.offset.x + dSizeX * (h.r * pivX - h.l * (1.f - pivX));
        newOffY = start.offset.y + dSizeY * (h.b * pivY - h.t * (1.f - pivY));
    }
    if (o.quantise)
    {
        newOffX = std::round(newOffX);
        newOffY = std::round(newOffY);
    }
    out.size   = { newSizeX, newSizeY };
    out.offset = { newOffX, newOffY };
    return out;
}

float SnapAxis(float& d, const float (&self)[3], const float* targets, int count, float threshold)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < count; ++j)
            if (std::fabs(self[i] + d - targets[j]) <= threshold)
            {
                d += targets[j] - (self[i] + d);
                return targets[j];
            }
    return std::numeric_limits<float>::quiet_NaN();
}

UI::UIRect SetAnchorsKeepRect(const UI::UIRect& r, UI::Vec2 aMin, UI::Vec2 aMax, const UI::Rect& resolved,
                              const Ctx& c)
{
    UI::UIRect n = r;
    n.anchorMin = aMin;
    n.anchorMax = aMax;
    return KeepRect(n, resolved, c);
}

UI::UIRect SetPivotKeepRect(const UI::UIRect& r, UI::Vec2 pivot, const UI::Rect& resolved, const Ctx& c)
{
    UI::UIRect n = r;
    n.pivot = pivot;
    return KeepRect(n, resolved, c);
}

} // namespace Editor::UIRectEdit
